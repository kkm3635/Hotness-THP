#define _GNU_SOURCE
#include <stddef.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <assert.h>
#include <sys/mman.h>
#include <sched.h>
#include <sys/types.h>
#include <bits/stdc++.h>
#include <unordered_map>
#include "pebs.h"

#define PAGE_SHIFT 21

using namespace std;

static uint64_t g_last_hash = 0;
static size_t g_last_size = 0;

static uint64_t *g_shared_arr = NULL;
void set_shared_arr(uint64_t *p){
        g_shared_arr = p;
}

static unordered_map<uint64_t, uint64_t> vpn_cnt;
static constexpr size_t K_TOP = (30ULL << 30) >> 21;

// Top-K 변할 때만 shared_array에 업데이트
static inline uint64_t mix64(uint64_t x){
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
static inline uint64_t unordered_hash_span(const uint64_t* a, size_t n){
    uint64_t h = 0;
    for (size_t i=0; i<n; ++i) h ^= mix64(a[i]);   // XOR로 순서 독립
    return h;
}

// Top-K shared_array에 반영
static void publish_topk_to_shared() {
    if (!g_shared_arr) return;

    // 1) map -> vector<pair<vpn,count>>
    static std::vector<std::pair<uint64_t,uint64_t>> vec; // 재사용
    vec.clear();
    vec.reserve(vpn_cnt.size());
    for (auto &e : vpn_cnt) vec.emplace_back(e.first, e.second);

    // 2) value 기준 top-K만 추출 (정렬 불필요)
    const size_t k = std::min(K_TOP, vec.size());
    if (vec.size() > k) {
        std::nth_element(vec.begin(), vec.begin()+k, vec.end(),
            [](auto &a, auto &b){ return a.second > b.second; });
        vec.resize(k);
    }

    // 3) 키만 뽑아서 해시 계산 (순서 독립)
    static std::vector<uint64_t> keys; // 재사용
    keys.clear();
    keys.reserve(k);
    for (size_t i=0; i<k; ++i) keys.push_back(vec[i].first);

    uint64_t cur_hash = unordered_hash_span(keys.data(), keys.size());

    // 4) 변화 없으면 스킵
    if (cur_hash == g_last_hash && keys.size() == g_last_size) {
        return;
    }

    // 5) 공유 배열에 기록: [0]=seq(64b), [1..K_TOP]=vpn
    uint64_t *out = g_shared_arr;

    uint64_t seq = __atomic_load_n(&out[0], __ATOMIC_RELAXED);
    __atomic_store_n(&out[0], seq + 1, __ATOMIC_RELEASE);   // 홀수=갱신중

    size_t i = 0;
    for (; i < keys.size(); ++i) out[1 + i] = keys[i];
    for (; i < K_TOP; ++i)       out[1 + i] = 0;            // 남는 칸 0

    __atomic_store_n(&out[0], seq + 2, __ATOMIC_RELEASE);   // 짝수=완료

    // 6) 캐시 업데이트
    g_last_hash = cur_hash;
    g_last_size = keys.size();
}

///////////////////////////// Sampling //////////////////////////////////

#define PERF_PAGES  (1 + (1 << 8))

struct perf_sample {
  struct perf_event_header header;
  __u64 ip;
  __u32 pid, tid;    /* if PERF_SAMPLE_TID */
  __u64 addr;        /* if PERF_SAMPLE_ADDR */
  __u64 weight;      /* if PERF_SAMPLE_WEIGHT */
  /* __u64 data_src;    /\* if PERF_SAMPLE_DATA_SRC *\/ */
  __u64 phy_addr;
};

static pid_t perf_target_pid;
void set_target_pid(pid_t pid){
    perf_target_pid = pid;
}

// perf_event_open()를 사용하여 PEBS 활용하는 함수
long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                     int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu,
                   group_fd, flags);
}

/* pebs events */
/* Need to find the right event code for the specific processor */
#define DRAM_LLC_LOAD_MISS  0x1d3 // EventSel=D3H, UMask=01H
#define REMOTE_DRAM_LLC_LOAD_MISS   0x2d3 // EventSel=D3H, UMask=02H
#define NVM_REMOTE_LLC_LOAD_MISS   0x80d1 // EventSel=D3H, UMask=10H
#define ALL_STORES          0x82d0 // EventSel=D0H, UMask=82H
#define ALL_LOADS           0x81d0 // EventSel=D0H, UMask=81H

enum events {
        DRAMREAD = 0,
        REMOTEREAD = 1,
	CXL = 2,
        MEMSTORE = 3,
        MEMLOAD = 4,
        N_EVENTS
};

static uint64_t get_pebs_event(enum events e)
{
        switch (e) {
              case DRAMREAD:
                      return DRAM_LLC_LOAD_MISS;
              case REMOTEREAD:
                      return REMOTE_DRAM_LLC_LOAD_MISS;
              case MEMSTORE:
                      return ALL_STORES;
        //        case REMOTEREAD:
         //               return REMOTE_DRAM_LLC_LOAD_MISS;
                
	//	case MEMLOAD:
	//		return ALL_LOADS;
		default:
                        return N_EVENTS;
        }
}

#define pcount 30
/* only prime numbers */
static const unsigned int pebs_period_list[pcount] = {
    199,    // 200 - min
    293,    // 300
    401,    // 400
    499,    // 500
    599,    // 600
    701,    // 700
    797,    // 800
    907,    // 900
    997,    // 1000
    1201,   // 1200
    1399,   // 1400
    1601,   // 1600
    1801,   // 1800
    1999,   // 2000
    2503,   // 2500
    3001,   // 3000
    3499,   // 3500
    4001,   // 4000
    4507,   // 4507
    4999,   // 5000
    6007,   // 6000
    7001,   // 7000
    7993,   // 8000
    9001,   // 9000
    10007,  // 10000
    12007,  // 12000
    13999,  // 14000
    16001,  // 16000
    17989,  // 18000
    19997,  // 20000 - max
};
unsigned int inst_sample_period = 50007;

static inline unsigned long get_sample_period(unsigned long cur) {
        if (cur < 0)
                return 0;
        else if (cur < pcount)
                return pebs_period_list[cur];
        else
                return pebs_period_list[pcount - 1];
}

/* Basic manual: man page of perf_event_open() */
int pebs_open(uint64_t config, uint64_t config1, uint64_t type) {
        struct perf_event_attr pe;
        memset(&pe, 0, sizeof(struct perf_event_attr));

        pe.type = PERF_TYPE_RAW;
        pe.size = sizeof(struct perf_event_attr);
        pe.config = config;
        pe.config1 = config1;
        if (config == ALL_STORES || config == ALL_LOADS) // all_stores event is too frequent, make the sampling period longer
                pe.sample_period = inst_sample_period;
        else
                pe.sample_period = get_sample_period(1);
        pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_WEIGHT | PERF_SAMPLE_ADDR | PERF_SAMPLE_PHYS_ADDR ;
        pe.disabled = 0;
        pe.exclude_kernel = 1;
        pe.exclude_hv = 1;
        pe.exclude_callchain_kernel = 1;
        pe.exclude_callchain_user = 1;
        pe.precise_ip = 2;
        pe.enable_on_exec = 1;

        int event_fd = perf_event_open(&pe, perf_target_pid, -1, -1, 0);
        if (event_fd <= 0) {
                printf("event_fd made by perf_event_open(): %d\n", event_fd);
                perror("Error opening perf event");
                exit(EXIT_FAILURE);
        }
        return event_fd;
}

int pebs_init(int *event_list, struct perf_event_mmap_page **record_addr_list) {
        int event;
        size_t mmap_size = sysconf(_SC_PAGESIZE) * PERF_PAGES;
        for (event=0; event<N_EVENTS; event++) {
                if (get_pebs_event((enum events)event) == N_EVENTS) {
                        continue;
                }
                event_list[event] = pebs_open(get_pebs_event((enum events)event), 0, event);
                record_addr_list[event] = (struct perf_event_mmap_page*)mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, event_list[event], 0);
                if (record_addr_list[event] == MAP_FAILED) {
                    perror("perf mmap");
                    fprintf(stderr, "event %d: mmap_size=%zu\n", event, mmap_size);
                    record_addr_list[event] = NULL;
                }
        }
        return 0;
}

struct arg_sampler {
        int *event_list;
        struct perf_event_mmap_page **record_addr_list;
};

pthread_t sampler_thread;
int sampler_stop;
#define SAMP_PERIOD 3

void *sampler(void *arg) {
        struct arg_sampler *args = (struct arg_sampler*)arg;
        int *event_fd_list = args->event_list;
        struct perf_event_mmap_page **record_addr_list = args->record_addr_list;

        // declaration for sample
        struct perf_event_mmap_page *p;
        char *pbuf;
        struct perf_event_header *ph;
        struct perf_sample* ps;
        uint64_t data_offset;

        while (!sampler_stop) {
                for (int i=0; i<N_EVENTS; i++) {
                        /* Reading PEBS sample */
                        p = record_addr_list[i];
                        if(!p) continue;
                        data_offset = p->data_offset; // segfault
                        pbuf = (char*)p + data_offset;
                        __sync_synchronize();
                        if(p->data_head == p->data_tail) {
                                continue;
                        }
                        while (p->data_head != p->data_tail) {
                                ph = (struct perf_event_header*)(pbuf + (p->data_tail % p->data_size));
                                switch(ph->type) {
                                        case PERF_RECORD_SAMPLE:
                                                ps = (struct perf_sample*)ph;
                                                if(ps!= NULL) {
                                                        uint64_t vpn = ps->addr >> PAGE_SHIFT;
//                                                        printf("va: %ld\n",ps->addr);
                                                        ++vpn_cnt[vpn];

                                                } else {
                                                        printf("ps is NULL\n");
                                                }
                                                break;
                                        default:
                                                break;
                                }
                                p->data_tail = p->data_tail + ph->size;
                        }
                }
                sleep(SAMP_PERIOD);
                publish_topk_to_shared();
        }

        // close fd
        for (int i=0; i<N_EVENTS; i++) {
                close(event_fd_list[i]);
        }
        free(event_fd_list);
        free(record_addr_list);
        free(args);

        return NULL;
}

void sampler_init(void) {
        // pebs event list initialization
        int *event_list = (int*)malloc(sizeof(int) * N_EVENTS); // container of fd
        memset(event_list, 0, sizeof(int) * N_EVENTS);

        struct perf_event_mmap_page **record_addr_list = (struct perf_event_mmap_page**)malloc(sizeof(struct perf_event_mmap_page*) * N_EVENTS);
        memset(record_addr_list, 0, sizeof(struct perf_event_mmap_page*) * N_EVENTS);

        pebs_init(event_list, record_addr_list); // perf_event_open

        struct arg_sampler* sampler_args = (struct arg_sampler*)malloc(sizeof(struct arg_sampler));
        sampler_args->event_list = event_list;
        sampler_args->record_addr_list = record_addr_list;

        // sampler thread creation
        int ret;
        ret = pthread_create(&sampler_thread, NULL, sampler, (void*)sampler_args);
        if (ret != 0) {
                perror("pthread create");
                exit(EXIT_FAILURE);
        }
        return;
}

void sampler_deinit(void) {
        sampler_stop = 1;
        pthread_join(sampler_thread, NULL);
}

