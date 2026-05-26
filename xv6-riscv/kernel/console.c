//
// Console input and output, to the uart.
// Reads are line at a time.
// Implements special input characters:
//   newline -- end of line
//   control-h -- backspace
//   control-u -- kill line
//   control-d -- end of file
//   control-p -- print process list
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

#define BACKSPACE 0x100  // erase the last output character
#define C(x)  ((x)-'@')  // Control-x

//
// send one character to the uart, but don't use
// interrupts or sleep(). safe to be called from
// interrupts, e.g. by printf and to echo input
// characters.
//
void
consputc(int c)
{
  if(c == BACKSPACE){
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b'); uartputc_sync(' '); uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

struct {
  struct spinlock lock;

  // input circular buffer (large enough for ASK/CACHE_SET lines with ~1KB args)
#define INPUT_BUF_SIZE 2048
  char buf[INPUT_BUF_SIZE];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
} cons;

// 단일 write() 시스템콜이 atomic 하게 콘솔에 나가도록 직렬화하는 sleeplock.
// (cons.lock 은 input edit/echo 까지 잡고 있어서 write 와 같은 락을 쓰면 입력
// 처리 deadlock 위험. write 전용 sleeplock 으로 분리.)
static struct sleeplock conswr_lock;

// Mirror buffer for sniffing agent commands. Holds the chars of the line
// currently being typed (between cons.w and cons.e). On newline, if the line
// starts with "REQ|" it is consumed by agent_dispatch and hidden from the
// shell. Kept in lockstep with cons.e via the same edit/erase paths.
#define AGENT_BUF_SIZE 2048
static char agent_buf[AGENT_BUF_SIZE];
static int  agent_len = 0;

//
// user write() system calls to the console go here.
// uses sleep() and UART interrupts.
//
int
consolewrite(int user_src, uint64 src, int n)
{
  char buf[32]; // move batches from user space to uart.
  int i = 0;

  // 한 번의 write() 호출이 통째로 콘솔에 나가도록 sleeplock 으로 직렬화.
  // 그렇지 않으면 동시 printf 한 부모/자식들의 출력이 char 단위로 byte-race.
  acquiresleep(&conswr_lock);

  while(i < n){
    int nn = sizeof(buf);
    if(nn > n - i)
      nn = n - i;
    if(either_copyin(buf, user_src, src+i, nn) == -1)
      break;
    uartwrite(buf, nn);
    i += nn;
  }

  releasesleep(&conswr_lock);
  return i;
}

//
// user read()s from the console go here.
// copy (up to) a whole input line to dst.
// user_dst indicates whether dst is a user
// or kernel address.
//
int
consoleread(int user_dst, uint64 dst, int n)
{
  uint target;
  int c;
  char cbuf;

  target = n;
  acquire(&cons.lock);
  while(n > 0){
    // wait until interrupt handler has put some
    // input into cons.buffer.
    while(cons.r == cons.w){
      if(killed(myproc())){
        release(&cons.lock);
        return -1;
      }
      // Drain the agent dispatcher queue here: valid proc context, cons.lock
      // released so cache handlers may sleep on begin_op().
      release(&cons.lock);
      agent_drain();
      acquire(&cons.lock);
      if(cons.r != cons.w) break;   // re-check: drain may have produced output
      sleep(&cons.r, &cons.lock);
    }

    c = cons.buf[cons.r++ % INPUT_BUF_SIZE];

    if(c == C('D')){  // end-of-file
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        cons.r--;
      }
      break;
    }

    // copy the input byte to the user-space buffer.
    cbuf = c;
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    if(c == '\n'){
      // a whole line has arrived, return to
      // the user-level read().
      break;
    }
  }
  release(&cons.lock);

  return target - n;
}

//
// the console input interrupt handler.
// uartintr() calls this for each input character.
// do erase/kill processing, append to cons.buf,
// wake up consoleread() if a whole line has arrived.
//
void
consoleintr(int c)
{
  acquire(&cons.lock);

  switch(c){
  case C('P'):  // Print process list.
    procdump();
    break;
  case C('U'):  // Kill line.
    while(cons.e != cons.w &&
          cons.buf[(cons.e-1) % INPUT_BUF_SIZE] != '\n'){
      cons.e--;
      consputc(BACKSPACE);
    }
    agent_len = 0;
    break;
  case C('H'): // Backspace
  case '\x7f': // Delete key
    if(cons.e != cons.w){
      cons.e--;
      consputc(BACKSPACE);
      if(agent_len > 0) agent_len--;
    }
    break;
  default:
    if(c != 0 && cons.e-cons.r < INPUT_BUF_SIZE){
      c = (c == '\r') ? '\n' : c;

      // echo back to the user.
      consputc(c);

      // store for consumption by consoleread().
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

      // mirror into the agent sniff buffer (skip overflow: too long to be cmd).
      if(agent_len < AGENT_BUF_SIZE-1) agent_buf[agent_len++] = c;

      if(c == '\n' || c == C('D') || cons.e-cons.r == INPUT_BUF_SIZE){
        // Try agent dispatch: if line is "REQ|...\n", consume it and hide
        // from the shell. Rewinding cons.e to cons.w drops the in-progress
        // line from the read buffer. consoleread() never sees it.
        if(agent_len > 4 && agent_buf[0]=='R' && agent_buf[1]=='E' &&
           agent_buf[2]=='Q' && agent_buf[3]=='|'){
          if(agent_buf[agent_len-1] == '\n') agent_len--;
          agent_buf[agent_len] = 0;
          cons.e = cons.w;
          release(&cons.lock);
          agent_dispatch(agent_buf);        // enqueue (interrupt-safe)
          acquire(&cons.lock);
          agent_len = 0;
          // Wake a reader so it gives us a process-context window to drain the
          // queue (consoleread() finds cons.r==cons.w, drains, then re-sleeps).
          wakeup(&cons.r);
          release(&cons.lock);
          return;
        }
        agent_len = 0;
        // wake up consoleread() if a whole line (or end-of-file) has arrived.
        cons.w = cons.e;
        wakeup(&cons.r);
      }
    }
    break;
  }
  
  release(&cons.lock);
}

void
consoleinit(void)
{
  initlock(&cons.lock, "cons");
  initsleeplock(&conswr_lock, "conswr");

  uartinit();

  // connect read and write system calls
  // to consoleread and consolewrite.
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
}
