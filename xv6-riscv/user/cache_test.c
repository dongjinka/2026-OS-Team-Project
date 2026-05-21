// Phase 3 — LLM cache 단독 시연 테스트 (userspace).
//
// 목적: 커널 캐시(syscalls set_cache/get_cache)가 RAM hit / RAM full evict /
// 디스크 promote 의 3가지 경로를 모두 수행하는지 확인.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define CACHE_SLOTS 16
#define VAL_MAX     256

static int pass = 0, fail = 0;

static void
check(const char *what, int cond)
{
  if(cond){
    printf("  PASS  %s\n", what);
    pass++;
  } else {
    printf("  FAIL  %s\n", what);
    fail++;
  }
}

static int
mystrlen(const char *s)
{
  int n = 0;
  while(s[n]) n++;
  return n;
}

static int
mystrcmp(const char *a, const char *b, int n)
{
  for(int i = 0; i < n; i++) if(a[i] != b[i]) return -1;
  return 0;
}

int
main(void)
{
  char buf[VAL_MAX];

  printf("=== Cache Test ===\n");

  // 1. 기본 set + get
  printf("[1] basic set/get\n");
  int r = set_cache("foo", 3, "bar", 3);
  check("set('foo','bar')", r == 0);
  int n = get_cache("foo", 3, buf, sizeof(buf));
  check("get('foo') returns vlen=3", n == 3);
  check("get('foo') value is 'bar'", n == 3 && mystrcmp(buf, "bar", 3) == 0);

  // 2. miss
  printf("[2] miss\n");
  n = get_cache("nonexistent", 11, buf, sizeof(buf));
  check("get('nonexistent') == -1", n == -1);

  // 3. overwrite
  printf("[3] overwrite\n");
  set_cache("foo", 3, "baz", 3);
  n = get_cache("foo", 3, buf, sizeof(buf));
  check("get('foo') now returns 'baz'", n == 3 && mystrcmp(buf, "baz", 3) == 0);

  // 4. RAM 가득 채우기 + LRU eviction (디스크로 떨어짐)
  printf("[4] fill RAM + force eviction\n");
  char k[16], v[16];
  for(int i = 0; i < CACHE_SLOTS + 4; i++){
    int klen = 0, vlen = 0;
    // 키: "k00", "k01", ... "k19"
    k[klen++] = 'k';
    k[klen++] = '0' + (i / 10);
    k[klen++] = '0' + (i % 10);
    v[vlen++] = 'v';
    v[vlen++] = '0' + (i / 10);
    v[vlen++] = '0' + (i % 10);
    set_cache(k, klen, v, vlen);
  }
  // 'foo'는 LRU 정책상 진작에 evict됐을 가능성 — 디스크에서 회수
  n = get_cache("foo", 3, buf, sizeof(buf));
  check("get('foo') after eviction (disk promote)",
        n == 3 && mystrcmp(buf, "baz", 3) == 0);

  // 5. 긴 키 (해싱 검증)
  printf("[5] long key (hash test)\n");
  const char *longkey = "This is a very long natural-language question that "
                        "would never fit in 128 bytes if we stored it raw, "
                        "but the FNV-1a 64-bit hash compresses it to 8 bytes.";
  int lk = mystrlen(longkey);
  r = set_cache(longkey, lk, "long-val", 8);
  check("set(longkey) succeeds", r == 0);
  n = get_cache(longkey, lk, buf, sizeof(buf));
  check("get(longkey) returns 'long-val'",
        n == 8 && mystrcmp(buf, "long-val", 8) == 0);

  // 6. 잘못된 입력
  printf("[6] bad args\n");
  check("set(NULL... klen=0) rejected", set_cache("x", 0, "v", 1) == -1);
  check("set(vlen=257) rejected", set_cache("x", 1, buf, 257) == -1);

  // 7. semantic — 단어 순서 바뀜 (semantic fallback 으로 hit)
  printf("[7] semantic — word reorder\n");
  const char *s7a = "the quick brown fox jumps over";
  const char *s7b = "quick the brown fox jumps over";
  set_cache(s7a, mystrlen(s7a), "CHAT|JUMPS", 10);
  n = get_cache(s7b, mystrlen(s7b), buf, sizeof(buf));
  check("reorder hit via semantic fallback",
        n >= 5 && mystrcmp(buf, "CHAT|", 5) == 0);

  // 8. semantic — 한국어 미세 차이 (물음표 유무)
  printf("[8] semantic — Korean paraphrase\n");
  const char *s8a = "오늘 날씨가 어때?";
  const char *s8b = "오늘 날씨가 어때";
  set_cache(s8a, mystrlen(s8a), "CHAT|맑음", 11);
  n = get_cache(s8b, mystrlen(s8b), buf, sizeof(buf));
  check("Korean punctuation drop hit via semantic",
        n >= 5 && mystrcmp(buf, "CHAT|", 5) == 0);

  // 9. negative — 무관 키는 semantic 으로도 매치 안 됨
  printf("[9] negative — unrelated keys do not match\n");
  const char *s9a = "apple banana cherry pineapple";
  const char *s9b = "kernel scheduler trap handler";
  set_cache(s9a, mystrlen(s9a), "CHAT|FRUIT", 10);
  n = get_cache(s9b, mystrlen(s9b), buf, sizeof(buf));
  check("unrelated keys → miss (no false positive)", n < 0);

  printf("\n=== %d PASS / %d FAIL ===\n", pass, fail);
  exit(fail == 0 ? 0 : 1);
}
