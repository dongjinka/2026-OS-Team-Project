// Phase 3 — LLM 응답 캐시 + 디스크 스왑 + 의미 매칭 (MinHash + Jaccard)
//
// 구조: 16-슬롯 RAM 테이블 + /cache.bin 디스크 오버레이.
// 키는 FNV-1a 64-bit 해시로 압축 (원문 미저장) → 임의 길이 프롬프트 수용.
// LRU 정책: full RAM 에 set 시 가장 오래된 슬롯을 디스크에 append.
// 디스크 hit 시 RAM 으로 promote (RAM-only 인라인 — cache_set 재귀 없음).
//
// Semantic cache: 정확 매치(FNV-1a) 미스 시 MinHash signature 들의 Jaccard
// 유사도로 paraphrase / 어순변경 / 부분 치환을 탐지. signature 는 RAM 슬롯에만
// 저장 (디스크 record 포맷 무변경) — 디스크 swap-out 항목은 정확 매치만 가능.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

#define CACHE_SLOTS    16
#define CACHE_VAL      1024
#define DISK_MAX_BYTES (4 * 1024 * 1024)  // /cache.bin 상한 4 MB

// MinHash + Jaccard 파라미터
#define SIG_K                       64    // signature 차원
#define SHINGLE_N                   3     // byte n-gram (UTF-8 한글 1글자=3B)
#define SEMANTIC_MATCH_NUM          7     // 임계 정수비 7/10 → Jaccard 0.7
#define SEMANTIC_MATCH_DEN          10
#define MIN_SHINGLES_FOR_SEMANTIC   4     // 너무 짧으면 signature 안 만듦

struct cache_entry {
  uint64 hash;                 // 0 = 빈 슬롯
  uint   last_access;          // ticks
  ushort vlen;
  ushort has_sig;              // 0 = signature 없음 (짧은 키 또는 디스크 promote)
  uint64 sig[SIG_K];           // MinHash signature — RAM only
  char   val[CACHE_VAL];
};

struct disk_record {
  uint64 hash;
  ushort vlen;
  char   val[CACHE_VAL];
};

static struct cache_entry cache_ram[CACHE_SLOTS];
static struct spinlock cache_lock;
static char cache_path[] = "/cache.bin";

// 디스크 I/O 전용 sleeplock — disk_scan / disk_append 가 공유하는 static
// 버퍼(rec_buf, tmp_val_buf)를 직렬화한다. begin_op 가 sleep 가능해
// sleeplock 사용.
static struct sleeplock disk_io_lock;
static char  tmp_val_buf[CACHE_VAL];     // cache_get_exact RAM-miss path
static struct disk_record rec_buf;       // disk_scan / disk_append 공유

extern uint ticks;

void
cacheinit(void)
{
  initlock(&cache_lock, "cache");
  initsleeplock(&disk_io_lock, "cache_disk");
}

// FNV-1a 64-bit. 0은 "빈 슬롯" 마커로 예약했으므로 충돌 시 1로 승격.
static uint64
fnv1a_64(const char *data, int len)
{
  uint64 h = 0xcbf29ce484222325ULL;
  for(int i = 0; i < len; i++){
    h ^= (uint64)(unsigned char)data[i];
    h *= 0x100000001b3ULL;
  }
  return (h == 0) ? 1 : h;
}

// MinHash 용 두 개의 독립 FNV-1a (다른 offset basis).
// double-hashing 의 base h1 / h2 — `g_k(s) = h1 + k*h2` 로 K개 변형 생성.
static void
fnv1a_64_pair(const char *s, int n, uint64 *h1, uint64 *h2)
{
  uint64 a = 0xcbf29ce484222325ULL;
  uint64 b = 0x84222325cbf29ce4ULL;     // 첫 basis 의 32비트 swap
  for(int i = 0; i < n; i++){
    a ^= (uint64)(unsigned char)s[i]; a *= 0x100000001b3ULL;
    b ^= (uint64)(unsigned char)s[i]; b *= 0x100000001b3ULL;
  }
  *h1 = (a == 0) ? 1 : a;
  *h2 = (b == 0) ? 1 : b;
}

// out_sig[SIG_K] 에 MinHash signature 작성. 반환: shingle 개수 (0이면 너무 짧음).
static int
build_signature(const char *key, int klen, uint64 out_sig[SIG_K])
{
  if(klen < SHINGLE_N) return 0;
  int n_shingle = klen - SHINGLE_N + 1;
  if(n_shingle < MIN_SHINGLES_FOR_SEMANTIC) return 0;

  for(int k = 0; k < SIG_K; k++) out_sig[k] = ~(uint64)0;

  for(int i = 0; i < n_shingle; i++){
    uint64 h1, h2;
    fnv1a_64_pair(key + i, SHINGLE_N, &h1, &h2);
    for(int k = 0; k < SIG_K; k++){
      uint64 g = h1 + (uint64)k * h2;
      if(g == 0) g = 1;
      if(g < out_sig[k]) out_sig[k] = g;
    }
  }
  return n_shingle;
}

// 두 signature 동일 위치 일치 개수 = 추정 Jaccard × SIG_K
static int
jaccard_match_count(const uint64 a[SIG_K], const uint64 b[SIG_K])
{
  int c = 0;
  for(int k = 0; k < SIG_K; k++) if(a[k] == b[k]) c++;
  return c;
}

// /cache.bin 순차 스캔 — hash 일치 레코드 찾기.
// CALLER MUST HOLD disk_io_lock — rec_buf 가 공유 static.
static int
disk_scan(uint64 hash, char *valbuf, int vbuflen)
{
  struct inode *ip;
  int found = -1;

  begin_op();
  if((ip = namei(cache_path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  uint sz = ip->size;
  for(uint off = 0; off + sizeof(rec_buf) <= sz; off += sizeof(rec_buf)){
    if(readi(ip, 0, (uint64)&rec_buf, off, sizeof(rec_buf)) != sizeof(rec_buf))
      break;
    if(rec_buf.hash == hash){
      int n = (rec_buf.vlen < vbuflen) ? rec_buf.vlen : vbuflen;
      memmove(valbuf, rec_buf.val, n);
      found = rec_buf.vlen;
      break;
    }
  }
  iunlockput(ip);
  end_op();
  return found;
}

// /cache.bin 끝에 레코드 append. 크기 상한 초과 시 silently fail (-1).
// CALLER MUST HOLD disk_io_lock — rec_buf 가 공유 static.
static int
disk_append(uint64 hash, const char *val, int vlen)
{
  struct inode *ip;

  if(vlen < 0 || vlen > CACHE_VAL) return -1;
  rec_buf.hash = hash;
  rec_buf.vlen = (ushort)vlen;
  memmove(rec_buf.val, val, vlen);
  if(vlen < CACHE_VAL)
    memset(rec_buf.val + vlen, 0, CACHE_VAL - vlen);

  begin_op();
  ip = namei(cache_path);
  if(ip == 0){
    ip = create(cache_path, T_FILE, 0, 0);
    if(ip == 0){ end_op(); return -1; }
  } else {
    ilock(ip);
  }
  if(ip->size + sizeof(rec_buf) > DISK_MAX_BYTES){
    iunlockput(ip); end_op();
    return -1;
  }
  int n = writei(ip, 0, (uint64)&rec_buf, ip->size, sizeof(rec_buf));
  iunlockput(ip);
  end_op();
  return (n == sizeof(rec_buf)) ? 0 : -1;
}

// RAM 슬롯에서 hash 일치 위치. 없으면 -1. caller가 cache_lock 보유 가정.
static int
ram_find(uint64 hash)
{
  for(int i = 0; i < CACHE_SLOTS; i++)
    if(cache_ram[i].hash == hash) return i;
  return -1;
}

int
cache_set(const char *key, int klen, const char *val, int vlen)
{
  if(klen <= 0 || vlen < 0 || vlen > CACHE_VAL) return -1;
  uint64 hash = fnv1a_64(key, klen);

  uint64 evict_hash = 0;
  char   evict_val[CACHE_VAL];
  ushort evict_vlen = 0;

  acquire(&cache_lock);

  int i = ram_find(hash);
  if(i < 0){
    for(int j = 0; j < CACHE_SLOTS; j++){
      if(cache_ram[j].hash == 0){ i = j; break; }
    }
  }
  if(i < 0){
    int lru = 0;
    uint lru_tick = cache_ram[0].last_access;
    for(int j = 1; j < CACHE_SLOTS; j++){
      if(cache_ram[j].last_access < lru_tick){
        lru_tick = cache_ram[j].last_access;
        lru = j;
      }
    }
    evict_hash = cache_ram[lru].hash;
    evict_vlen = cache_ram[lru].vlen;
    memmove(evict_val, cache_ram[lru].val, evict_vlen);
    i = lru;
  }

  cache_ram[i].hash = hash;
  cache_ram[i].vlen = (ushort)vlen;
  cache_ram[i].last_access = ticks;
  memmove(cache_ram[i].val, val, vlen);
  // signature 를 슬롯에 직접 작성 — stack 임시 K-array 회피해 frame 절약.
  int ns = build_signature(key, klen, cache_ram[i].sig);
  cache_ram[i].has_sig = (ns > 0) ? 1 : 0;

  release(&cache_lock);

  if(evict_hash != 0){
    acquiresleep(&disk_io_lock);
    disk_append(evict_hash, evict_val, evict_vlen);
    releasesleep(&disk_io_lock);
  }

  return 0;
}

// 정확 매치 — RAM 탐색 → 디스크 fallback (기존 cache_get 본체).
int
cache_get_exact(const char *key, int klen, char *valbuf, int vbuflen)
{
  if(klen <= 0 || vbuflen <= 0) return -1;
  uint64 hash = fnv1a_64(key, klen);

  acquire(&cache_lock);
  int i = ram_find(hash);
  if(i >= 0){
    int n = (cache_ram[i].vlen < vbuflen) ? cache_ram[i].vlen : vbuflen;
    memmove(valbuf, cache_ram[i].val, n);
    cache_ram[i].last_access = ticks;
    int vlen = cache_ram[i].vlen;
    release(&cache_lock);
    return vlen;
  }
  release(&cache_lock);

  acquiresleep(&disk_io_lock);
  int vlen = disk_scan(hash, tmp_val_buf, CACHE_VAL);
  if(vlen < 0){
    releasesleep(&disk_io_lock);
    return -1;
  }

  int n = (vlen < vbuflen) ? vlen : vbuflen;
  memmove(valbuf, tmp_val_buf, n);

  // RAM promote — 빈 슬롯 있을 때만. 원본 key 가 없으므로 signature 재계산 불가
  // → has_sig=0.
  int copy_vlen = (vlen > CACHE_VAL) ? CACHE_VAL : vlen;
  acquire(&cache_lock);
  if(ram_find(hash) < 0){
    for(int j = 0; j < CACHE_SLOTS; j++){
      if(cache_ram[j].hash == 0){
        cache_ram[j].hash = hash;
        cache_ram[j].vlen = (ushort)copy_vlen;
        cache_ram[j].last_access = ticks;
        cache_ram[j].has_sig = 0;
        memmove(cache_ram[j].val, tmp_val_buf, copy_vlen);
        break;
      }
    }
  }
  release(&cache_lock);

  releasesleep(&disk_io_lock);
  return vlen;
}

// 의미 매치 — RAM 16 슬롯의 signature 와 query signature 의 Jaccard 유사도.
// out_score: 최고 매치 개수 (0..SIG_K) — caller 로깅용. miss 시 0.
int
cache_get_semantic(const char *key, int klen, char *valbuf, int vbuflen,
                   int *out_score)
{
  if(out_score) *out_score = 0;
  if(klen <= 0 || vbuflen <= 0) return -1;

  uint64 qsig[SIG_K];
  int n_shingle = build_signature(key, klen, qsig);
  if(n_shingle == 0) return -1;

  acquire(&cache_lock);
  int best = -1;
  int best_match = -1;
  uint best_tick = 0;
  for(int j = 0; j < CACHE_SLOTS; j++){
    if(cache_ram[j].hash == 0 || cache_ram[j].has_sig == 0) continue;
    int m = jaccard_match_count(qsig, cache_ram[j].sig);
    if(m > best_match ||
       (m == best_match && cache_ram[j].last_access > best_tick)){
      best_match = m;
      best = j;
      best_tick = cache_ram[j].last_access;
    }
  }

  // match/SIG_K ≥ NUM/DEN  ⇔  match*DEN ≥ NUM*SIG_K (정수 비교)
  if(best < 0 ||
     best_match * SEMANTIC_MATCH_DEN < SEMANTIC_MATCH_NUM * SIG_K){
    release(&cache_lock);
    return -1;
  }

  int n = (cache_ram[best].vlen < vbuflen) ? cache_ram[best].vlen : vbuflen;
  memmove(valbuf, cache_ram[best].val, n);
  cache_ram[best].last_access = ticks;
  int vlen = cache_ram[best].vlen;
  release(&cache_lock);

  if(out_score) *out_score = best_match;
  return vlen;
}

// 호환 wrapper — sys_get_cache, handle_cache_get 가 정확 → 의미 폴백을 자동 수신.
// handle_ask 는 분리된 두 함수를 직접 호출해 [cache] HIT vs SEMANTIC HIT 를 구분.
int
cache_get(const char *key, int klen, char *valbuf, int vbuflen)
{
  int v = cache_get_exact(key, klen, valbuf, vbuflen);
  if(v >= 0) return v;
  int score;
  return cache_get_semantic(key, klen, valbuf, vbuflen, &score);
}
