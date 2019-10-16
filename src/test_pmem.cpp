#include <gflags/gflags.h>
#include <immintrin.h>
#include <sys/time.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include "../util/System.hpp"
#include "../util/random.h"
#include "../util/uniform.hpp"
#include "./CCEH/CCEH_baseline.h"
#include "./Level/level.h"
#include "allocator.h"
#include "ex_finger.h"
#include "lh_finger.h"
#include "libpmemobj.h"
#include "utils.h"

std::string pool_name = "/mnt/pmem0/";
// static const char *pool_name = "pmem_hash.data";
static const size_t pool_size = 1024ul * 1024ul * 1024ul * 30ul;
DEFINE_string(index, "dash-ex",
              "which index to evaluate:dash-ex/dash-lh/cceh/level");
DEFINE_string(k, "fixed", "the type of stored keys: fixed/variable");
DEFINE_uint64(i, 64, "the initial number of segments in extendible hashing");
DEFINE_uint64(t, 1, "the number of concurrent threads");
DEFINE_uint64(n, 0, "the number of pre-insertion load");
DEFINE_uint64(p, 20000000,
              "the number of operations(insert/search/deletion) to execute");
DEFINE_string(op, "full",
              "which type of operation to execute:insert/pos/neg/delete/mixed");
DEFINE_double(r, 1, "read ratio for mixed workload");
DEFINE_double(s, 0, "insert ratio for mixed workload");
DEFINE_double(d, 0, "delete ratio for mixed workload");
DEFINE_bool(e, true, "whether register epoch in application level");

uint64_t initCap, thread_num, load_num, operation_num;
std::string operation;
std::string key_type;
std::string index_type;
int bar_a, bar_b, bar_c;
double read_ratio, insert_ratio, delete_ratio;
std::mutex mtx;
std::condition_variable cv;
bool finished = false;
bool open_epoch;
struct timeval tv1, tv2, tv3;

struct range {
  int index;
  uint64_t begin;
  uint64_t end;
  int length; /*if this is the variable lenght key, use this parameter to
                 indicate the length of the key*/
  void *workload;
  uint64_t random_num;
};

void set_affinity(uint32_t idx) {
  cpu_set_t my_set;
  CPU_ZERO(&my_set);
  if (idx < 24) {
    CPU_SET(idx, &my_set);
  } else {
    CPU_SET(idx + 24, &my_set);
  }
  sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
}

template <class T>
Hash<T> *InitializeIndex(int seg_num) {
  Hash<T> *eh;
  bool file_exist = false;
  if (index_type == "dash-ex") {
    std::string index_pool_name = pool_name + "pmem_ex.data";
    if (FileExists(index_pool_name.c_str())) file_exist = true;
    Allocator::Initialize(index_pool_name.c_str(), pool_size);

    std::cout << "pool addr is " << Allocator::Get()->pm_pool_ << std::endl;
    std::cout << "Initialize DASH-Extendible Hashing" << std::endl;
#ifdef PREALLOC
    extendible::TlsTablePool<Key_t>::Initialize();
#endif
    eh = reinterpret_cast<Hash<T> *>(
        Allocator::GetRoot(sizeof(extendible::Finger_EH<T>)));
    if (!file_exist) {
      new (eh) extendible::Finger_EH<T>(seg_num, Allocator::Get()->pm_pool_);
    } else {
      new (eh) extendible::Finger_EH<T>();
    }

  } else if (index_type == "dash-lh") {
    std::string index_pool_name = pool_name + "pmem_lh.data";
    if (FileExists(index_pool_name.c_str())) file_exist = true;
    Allocator::Initialize(index_pool_name.c_str(), pool_size);
    std::cout << "Initialize DASH-Linear Hashing" << std::endl;
#ifdef PREALLOC
    linear::TlsTablePool<Key_t>::Initialize();
#endif
    eh = reinterpret_cast<Hash<T> *>(
        Allocator::GetRoot(sizeof(linear::Linear<T>)));
    if (!file_exist) {
      new (eh) linear::Linear<T>(Allocator::Get()->pm_pool_);
    } else {
      new (eh) linear::Linear<T>();
    }
  } else if (index_type == "cceh") {
    std::string index_pool_name = pool_name + "pmem_cceh.data";
    if (FileExists(index_pool_name.c_str())) file_exist = true;
    Allocator::Initialize(index_pool_name.c_str(), pool_size);
    std::cout << "Initialize CCEH" << std::endl;
    eh = reinterpret_cast<Hash<T> *>(Allocator::GetRoot(sizeof(cceh::CCEH<T>)));
    if (!file_exist) {
      new (eh) cceh::CCEH<T>(seg_num, Allocator::Get()->pm_pool_);
    } else {
      new (eh) cceh::CCEH<T>();
    }
    std::cout << "Finish the initialization of CCEH" << std::endl;
  } else if (index_type == "level") {
    std::string index_pool_name = pool_name + "pmem_level.data";
    if (FileExists(index_pool_name.c_str())) file_exist = true;
    Allocator::Initialize(index_pool_name.c_str(), pool_size);
    std::cout << "Initialize Level Hashing" << std::endl;
    eh = reinterpret_cast<Hash<T> *>(
        Allocator::GetRoot(sizeof(level::LevelHashing<T>)));
    if (!file_exist) {
      new (eh) level::LevelHashing<T>();
      int level_size = 13;
      level::initialize_level(Allocator::Get()->pm_pool_,
                              reinterpret_cast<level::LevelHashing<T> *>(eh),
                              &level_size);
    } else {
      new (eh) level::LevelHashing<T>();
    }
  }
  return eh;
}

/*generate random 8-byte number and store it in the memory_region*/
void generate_8B(void *memory_region, int generate_num, bool persist) {
  uint64_t *array = reinterpret_cast<uint64_t *>(memory_region);

  for (uint64_t i = 0; i < generate_num; ++i) {
    array[i] = genrand64_int64();
  }

  if (persist) {
    Allocator::Persist(memory_region, generate_num * sizeof(uint64_t));
  }
}

/*generate random 16-byte string and store it in the memory_region*/
void generate_16B(void *memory_region, int generate_num, int length,
                  bool persist) {
  string_key *var_key;
  uint64_t *_key;
  char *workload = reinterpret_cast<char *>(memory_region);

  int word_num = (length / 8) + (((length % 8) != 0) ? 1 : 0);
  _key = reinterpret_cast<uint64_t *>(malloc(word_num * sizeof(uint64_t)));

  for (uint64_t i = 0; i < generate_num; ++i) {
    var_key = reinterpret_cast<string_key *>(workload +
                                             i * (length + sizeof(string_key)));
    var_key->length = length;
    for (int j = 0; j < word_num; ++j) {
      _key[j] = genrand64_int64();
    }
    memcpy(var_key->key, _key, length);
  }

  if (persist) {
    Allocator::Persist(memory_region,
                       generate_num * (sizeof(string_key) + length));
  }
}

template <class T>
void Load(int kv_num, Hash<T> *index, int length, void *workload) {
  if (kv_num == 0) return;
  std::string fixed("fixed");
  T *_worklod = reinterpret_cast<T *>(workload);
  T key;
  if constexpr (!std::is_pointer_v<T>) {
    for (uint64_t i = 0; i < kv_num; ++i) {
      index->Insert(_worklod[i], DEFAULT);
    }
  } else { /*genereate 16B key*/
    char *persist_workload = reinterpret_cast<char *>(workload);
    int string_key_size = sizeof(string_key) + length;
    for (uint64_t i = 0; i < kv_num; ++i) {
      key = reinterpret_cast<T>(persist_workload + i * string_key_size);
      index->Insert(key, DEFAULT);
    }
  }
  std::cout << "Finish loading " << kv_num << " keys" << std::endl;
}

inline void spin_wait() {
  SUB(&bar_b, 1);
  while (LOAD(&bar_a) == 1)
    ; /*spinning*/
}

inline void end_notify() {
  if (SUB(&bar_c, 1) == 0) {
    std::unique_lock<std::mutex> lck(mtx);
    finished = true;
    cv.notify_one();
  }
}

template <class T>
void concurr_insert_without_epoch(struct range *_range, Hash<T> *index) {
  set_affinity(_range->index);
  int begin = _range->begin;
  int end = _range->end;
  char *workload = reinterpret_cast<char *>(_range->workload);
  T key;

  spin_wait();
  // if(key_type == "fixed"){/*fixed length key benchmark*/
  if constexpr (!std::is_pointer_v<T>) {
    T *key_array = reinterpret_cast<T *>(workload);
    for (uint64_t i = begin; i < end; ++i) {
      index->Insert(key_array[i], DEFAULT);
    }
  } else {
    T var_key;
    uint64_t string_key_size = sizeof(string_key) + _range->length;
    for (uint64_t i = begin; i < end; ++i) {
      var_key = reinterpret_cast<T>(workload + string_key_size * i);
      index->Insert(var_key, DEFAULT);
    }
  }

  end_notify();
}

template <class T>
void concurr_insert(struct range *_range, Hash<T> *index) {
  set_affinity(_range->index);
  int begin = _range->begin;
  int end = _range->end;
  char *workload = reinterpret_cast<char *>(_range->workload);
  T key;

  if constexpr (!std::is_pointer_v<T>) {
    T *key_array = reinterpret_cast<T *>(workload);
    uint64_t round = (end - begin) / 1000;
    uint64_t i = 0;
    spin_wait();

    while (i < round) {
      auto epoch_guard = Allocator::AquireEpochGuard();
      uint64_t _end = begin + (i + 1) * 1000;
      for (uint64_t j = begin + i * 1000; j < _end; ++j) {
        index->Insert(key_array[j], DEFAULT, true);
      }
      ++i;
    }

    {
      auto epoch_guard = Allocator::AquireEpochGuard();
      for (i = begin + 1000 * round; i < end; ++i) {
        index->Insert(key_array[i], DEFAULT, true);
      }
    }
  } else {
    T var_key;
    uint64_t round = (end - begin) / 1000;
    uint64_t i = 0;
    uint64_t string_key_size = sizeof(string_key) + _range->length;

    spin_wait();
    while (i < round) {
      auto epoch_guard = Allocator::AquireEpochGuard();
      uint64_t _end = begin + (i + 1) * 1000;
      for (uint64_t j = begin + i * 1000; j < _end; ++j) {
        var_key = reinterpret_cast<T>(workload + string_key_size * j);
        index->Insert(var_key, DEFAULT, true);
      }
      ++i;
    }

    {
      auto epoch_guard = Allocator::AquireEpochGuard();
      for (i = begin + 1000 * round; i < end; ++i) {
        var_key = reinterpret_cast<T>(workload + string_key_size * i);
        index->Insert(var_key, DEFAULT, true);
      }
    }
  }
  end_notify();
}

template <class T>
void concurr_search(struct range *_range, Hash<T> *index) {
  set_affinity(_range->index);
  uint64_t begin = _range->begin;
  uint64_t end = _range->end;
  char *workload = reinterpret_cast<char *>(_range->workload);
  T key;
  uint64_t not_found = 0;

  if constexpr (!std::is_pointer_v<T>) {
    T *key_array = reinterpret_cast<T *>(workload);
    uint64_t round = (end - begin) / 1000;
    uint64_t i = 0;
    spin_wait();

    while (i < round) {
      auto epoch_guard = Allocator::AquireEpochGuard();
      uint64_t _end = begin + (i + 1) * 1000;
      for (uint64_t j = begin + i * 1000; j < _end; ++j) {
        if (index->Get(key_array[j], true) == NONE) not_found++;
      }
      ++i;
    }

    {
      auto epoch_guard = Allocator::AquireEpochGuard();
      for (i = begin + 1000 * round; i < end; ++i) {
        if (index->Get(key_array[i], true) == NONE) not_found++;
      }
    }
  } else {
    T var_key;
    uint64_t round = (end - begin) / 1000;
    uint64_t i = 0;
    uint64_t string_key_size = sizeof(string_key) + _range->length;

    spin_wait();
    while (i < round) {
      auto epoch_guard = Allocator::AquireEpochGuard();
      uint64_t _end = begin + (i + 1) * 1000;
      for (uint64_t j = begin + i * 1000; j < _end; ++j) {
        var_key = reinterpret_cast<T>(workload + string_key_size * j);
        if (index->Get(var_key, true) == NONE) not_found++;
      }
      ++i;
    }

    {
      auto epoch_guard = Allocator::AquireEpochGuard();
      for (i = begin + 1000 * round; i < end; ++i) {
        var_key = reinterpret_cast<T>(workload + string_key_size * i);
        if (index->Get(var_key, true) == NONE) not_found++;
      }
    }
  }
  end_notify();
}

template <class T>
void concurr_search_without_epcoh(struct range *_range, Hash<T> *index) {
  set_affinity(_range->index);
  int begin = _range->begin;
  int end = _range->end;
  char *workload = reinterpret_cast<char *>(_range->workload);
  T key;
  uint64_t not_found = 0;

  spin_wait();

  if constexpr (!std::is_pointer_v<T>) {
    T *key_array = reinterpret_cast<T *>(workload);
    for (uint64_t i = begin; i < end; ++i) {
      if (index->Get(key_array[i]) == NONE) {
        not_found++;
      }
    }
  } else {
    T var_key;
    uint64_t string_key_size = sizeof(string_key) + _range->length;
    for (uint64_t i = begin; i < end; ++i) {
      var_key = reinterpret_cast<T>(workload + string_key_size * i);
      if (index->Get(var_key) == NONE) {
        not_found++;
      }
    }
  }
  // std::cout << "not_found = " << not_found << std::endl;
  end_notify();
}

template <class T>
void concurr_delete_without_epoch(struct range *_range, Hash<T> *index) {
  set_affinity(_range->index);
  int begin = _range->begin;
  int end = _range->end;
  char *workload = reinterpret_cast<char *>(_range->workload);
  T key;
  uint64_t not_found = 0;

  spin_wait();
  if constexpr (!std::is_pointer_v<T>) {
    T *key_array = reinterpret_cast<T *>(workload);
    for (uint64_t i = begin; i < end; ++i) {
      if (index->Delete(key_array[i]) == false) {
        not_found++;
      }
    }
  } else {
    T var_key;
    int string_key_size = sizeof(string_key) + _range->length;
    for (uint64_t i = begin; i < end; ++i) {
      var_key = reinterpret_cast<T>(workload + string_key_size * i);
      if (index->Delete(var_key) == false) {
        not_found++;
      }
    }
  }
  std::cout << "not_found = " << not_found << std::endl;
  end_notify();
}

template <class T>
void concurr_delete(struct range *_range, Hash<T> *index) {
  set_affinity(_range->index);
  int begin = _range->begin;
  int end = _range->end;
  char *workload = reinterpret_cast<char *>(_range->workload);
  T key;

  if constexpr (!std::is_pointer_v<T>) {
    T *key_array = reinterpret_cast<T *>(workload);
    uint64_t round = (end - begin) / 1000;
    uint64_t i = 0;
    spin_wait();

    while (i < round) {
      auto epoch_guard = Allocator::AquireEpochGuard();
      uint64_t _end = begin + (i + 1) * 1000;
      for (uint64_t j = begin + i * 1000; j < _end; ++j) {
        index->Delete(key_array[j], true);
      }
      ++i;
    }

    {
      auto epoch_guard = Allocator::AquireEpochGuard();
      for (i = begin + 1000 * round; i < end; ++i) {
        index->Delete(key_array[i], true);
      }
    }
  } else {
    T var_key;
    uint64_t round = (end - begin) / 1000;
    uint64_t i = 0;
    uint64_t string_key_size = sizeof(string_key) + _range->length;

    spin_wait();
    while (i < round) {
      auto epoch_guard = Allocator::AquireEpochGuard();
      uint64_t _end = begin + (i + 1) * 1000;
      for (uint64_t j = begin + i * 1000; j < _end; ++j) {
        var_key = reinterpret_cast<T>(workload + string_key_size * j);
        index->Delete(var_key, true);
      }
      ++i;
    }

    {
      auto epoch_guard = Allocator::AquireEpochGuard();
      for (i = begin + 1000 * round; i < end; ++i) {
        var_key = reinterpret_cast<T>(workload + string_key_size * i);
        index->Delete(var_key, true);
      }
    }
  }
  end_notify();
}

template <class T>
void mixed_without_epoch(struct range *_range, Hash<T> *index) {
  set_affinity(_range->index);
  uint64_t begin = _range->begin;
  uint64_t end = _range->end;
  char *workload = reinterpret_cast<char *>(_range->workload);
  T *key_array = reinterpret_cast<T *>(_range->workload);
  T key;
  int string_key_size = sizeof(string_key) + _range->length;

  UniformRandom rng(_range->random_num);
  uint32_t random;
  uint32_t not_found = 0;

  uint32_t insert_sign = (uint32_t)(insert_ratio * 100);
  uint32_t read_sign = (uint32_t)(read_ratio * 100) + insert_sign;
  uint32_t delete_sign = (uint32_t)(delete_ratio * 100) + read_sign;
  std::cout << "insert sign = " << insert_sign << std::endl;
  std::cout << "read sign = " << read_sign << std::endl;

  spin_wait();

  for (uint64_t i = begin; i < end; ++i) {
    if constexpr (std::is_pointer_v<T>) { /* variable length*/
      key = reinterpret_cast<T>(workload + string_key_size * i);
    } else {
      key = key_array[i];
    }

    random = rng.next_uint32() % 100;
    if (random < insert_sign) { /*insert*/
      index->Insert(key, DEFAULT);
    } else if (random < read_sign) { /*get*/
      if (index->Get(key) == NONE) {
        not_found++;
      }
    } else { /*delete*/
      index->Delete(key);
    }
  }
  std::cout << "not_found = " << not_found << std::endl;
  /*the last thread notify the main thread to wake up*/
  end_notify();
}

template <class T>
void mixed(struct range *_range, Hash<T> *index) {
  set_affinity(_range->index);
  uint64_t begin = _range->begin;
  uint64_t end = _range->end;
  char *workload = reinterpret_cast<char *>(_range->workload);
  T *key_array = reinterpret_cast<T *>(_range->workload);
  T key;
  int string_key_size = sizeof(string_key) + _range->length;
  std::cout << "string_key_size = " << string_key_size << std::endl;

  UniformRandom rng(_range->random_num);
  uint32_t random;
  uint64_t not_found = 0;

  uint32_t insert_sign = (uint32_t)(insert_ratio * 100);
  uint32_t read_sign = (uint32_t)(read_ratio * 100) + insert_sign;
  uint32_t delete_sign = (uint32_t)(delete_ratio * 100) + read_sign;
  std::cout << "insert sign = " << insert_sign << std::endl;
  std::cout << "read sign = " << read_sign << std::endl;

  uint64_t round = (end - begin) / 1000;
  uint64_t i = 0;
  spin_wait();

  while (i < round) {
    auto epoch_guard = Allocator::AquireEpochGuard();
    uint64_t _end = begin + (i + 1) * 1000;
    for (uint64_t j = begin + i * 1000; j < _end; ++j) {
      if constexpr (std::is_pointer_v<T>) { /* variable length*/
        key = reinterpret_cast<T>(workload + string_key_size * j);
      } else {
        key = key_array[j];
      }

      random = rng.next_uint32() % 100;
      if (random < insert_sign) { /*insert*/
        index->Insert(key, DEFAULT, true);
      } else if (random < read_sign) { /*get*/
        if (index->Get(key, true) == NONE) {
          not_found++;
        }
      } else { /*delete*/
        index->Delete(key, true);
      }
    }
    ++i;
  }

  {
    auto epoch_guard = Allocator::AquireEpochGuard();
    for (i = begin + 1000 * round; i < end; ++i) {
      if constexpr (std::is_pointer_v<T>) { /* variable length*/
        key = reinterpret_cast<T>(workload + string_key_size * i);
      } else {
        key = key_array[i];
      }

      random = rng.next_uint32() % 100;
      if (random < insert_sign) { /*insert*/
        index->Insert(key, DEFAULT, true);
      } else if (random < read_sign) { /*get*/
        if (index->Get(key, true) == NONE) {
          not_found++;
        }
      } else { /*delete*/
        index->Delete(key, true);
      }
    }
  }

  std::cout << "not_found = " << not_found << std::endl;
  /*the last thread notify the main thread to wake up*/
  end_notify();
}

template <class T>
void GeneralBench(range *rarray, Hash<T> *index, int thread_num,
                  uint64_t operation_num, std::string profile_name,
                  void (*test_func)(struct range *, Hash<T> *)) {
  std::thread *thread_array[1024];
  profile_name = profile_name + std::to_string(thread_num);
  double duration;
  finished = false;
  bar_a = 1;
  bar_b = thread_num;
  bar_c = thread_num;

  std::cout << profile_name << " Begin" << std::endl;
  // System::profile(profile_name, [&]() {
  for (uint64_t i = 0; i < thread_num; ++i) {
    thread_array[i] = new std::thread(*test_func, &rarray[i], index);
  }

  while (LOAD(&bar_b) != 0)
    ;                                     // Spin
  std::unique_lock<std::mutex> lck(mtx);  // get the lock of condition variable

  gettimeofday(&tv1, NULL);
  STORE(&bar_a, 0);  // start test
  while (!finished) {
    cv.wait(lck);  // go to sleep and wait for the wake-up from child threads
  }
  gettimeofday(&tv2, NULL);  // test end

  for (int i = 0; i < thread_num; ++i) {
    thread_array[i]->join();
    delete thread_array[i];
  }
  duration = (double)(tv2.tv_usec - tv1.tv_usec) / 1000000 +
             (double)(tv2.tv_sec - tv1.tv_sec);
  printf(
      "%d threads, Time = %f s, throughput = %f "
      "ops/s\n",
      thread_num,
      (double)(tv2.tv_usec - tv1.tv_usec) / 1000000 +
          (double)(tv2.tv_sec - tv1.tv_sec),
      operation_num / duration);
  //});
  std::cout << profile_name << " End" << std::endl;
}

void *GenerateWorkload(int generate_num, int length) {
  /*Since there are both positive search and negative search, it should generate
   * 2 * generate_num workload*/
  void *workload;
  if (key_type == "fixed") {
    workload = malloc(generate_num * sizeof(uint64_t));
    generate_8B(workload, generate_num, false);
  } else { /*Generate the variable lengh workload*/
    std::cout << "Genereate workload for variable length key " << std::endl;
    workload = malloc(generate_num * (length + sizeof(string_key)));
    generate_16B(workload, generate_num, length, false);
    std::cout << "Finish Generation" << std::endl;
  }
  return workload;
}

template <class T>
void Run() {
  unsigned long long init[4] = {0x12345ULL, 0x23456ULL, 0x34567ULL, 0x45678ULL},
                     length = 4;
  init_by_array64(init, length); /*initialize random number generation*/
  /* Initialize Index for Finger_EH*/
  Hash<T> *index = InitializeIndex<T>(initCap);

  uint64_t generate_num = operation_num * 2 + load_num;
  /* Generate the workload and corresponding range array*/
  void *workload = GenerateWorkload(generate_num, 16);
  void *insert_workload;
  if (key_type != "fixed") {
    PMEMoid ptr;
    Allocator::ZAllocate(&ptr, kCacheLineSize,
                         (sizeof(string_key) + 16) * generate_num);
    insert_workload = pmemobj_direct(ptr);
    std::cout << "allocate finish for pm" << std::endl;
    memcpy(insert_workload, workload, (sizeof(string_key) + 16) * generate_num);
  } else {
    insert_workload = workload;
  }

  Load<T>(load_num, index, 16, insert_workload);
  void *not_used_workload;
  void *not_used_insert_workload;

  if (key_type == "fixed") {
    uint64_t *key_array = reinterpret_cast<uint64_t *>(workload);
    not_used_workload = reinterpret_cast<void *>(key_array + load_num);
    not_used_insert_workload = not_used_workload;
  } else {
    char *key_array = reinterpret_cast<char *>(workload);
    char *persist_key_array = reinterpret_cast<char *>(insert_workload);
    not_used_workload = key_array + (sizeof(string_key) + 16) * load_num;
    not_used_insert_workload =
        persist_key_array + (sizeof(string_key) + 16) * load_num;
  }

  /* Description of the workload*/
  srand((unsigned)time(NULL));
  struct range *rarray;
  uint64_t chunk_size = operation_num / thread_num;
  rarray = reinterpret_cast<range *>(malloc(thread_num * (sizeof(range))));
  for (int i = 0; i < thread_num; ++i) {
    rarray[i].index = i;
    rarray[i].random_num = rand();
    rarray[i].begin = i * chunk_size;
    rarray[i].end = (i + 1) * chunk_size;
    rarray[i].length = 16;
    rarray[i].workload = not_used_workload;
  }
  rarray[thread_num - 1].end = operation_num;

  /* Benchmark Phase */
  if (operation == "insert") {
    for (int i = 0; i < thread_num; ++i) {
      rarray[i].workload = not_used_insert_workload;
    }
    if (open_epoch == true) {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Insert",
                      &concurr_insert);
    } else {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Insert",
                      &concurr_insert_without_epoch);
    }
  } else if (operation == "pos") {
    if (!load_num) {
      std::cout << "Please first specify the # pre_load keys!" << std::endl;
      return;
    }
    for (int i = 0; i < thread_num; ++i) {
      rarray[i].workload = workload;
    }
    if (open_epoch == true) {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Pos_search",
                      &concurr_search);
    } else {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Pos_search",
                      &concurr_search_without_epcoh);
    }
  } else if (operation == "neg") {
    if (!load_num) {
      std::cout << "Please first specify the # pre_load keys!" << std::endl;
      return;
    }
    if (open_epoch == true) {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Neg_search",
                      &concurr_search);
    } else {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Neg_search",
                      &concurr_search_without_epcoh);
    }
  } else if (operation == "delete") {
    if (!load_num) {
      std::cout << "Please first specify the # pre_load keys!" << std::endl;
      return;
    }
    for (int i = 0; i < thread_num; ++i) {
      rarray[i].workload = workload;
    }
    if (open_epoch == true) {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Delete",
                      &concurr_delete);
    } else {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Delete",
                      &concurr_delete_without_epoch);
    }
  } else if (operation == "mixed") {
    for (int i = 0; i < thread_num; ++i) {
      rarray[i].workload = not_used_insert_workload;
    }
    if (open_epoch == true) {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Mixed",
                      &mixed);
    } else {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Mixed",
                      &mixed_without_epoch);
    }
  } else { /*do the benchmark for all single operations*/
    std::cout << "insertion start" << std::endl;
    for (int i = 0; i < thread_num; ++i) {
      rarray[i].workload = not_used_insert_workload;
    }
    if (open_epoch == true) {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Insert",
                      &concurr_insert);
    } else {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Insert",
                      &concurr_insert_without_epoch);
    }

    /*
    index->getNumber();

    gettimeofday(&tv1, NULL);
    index->Recovery();
    gettimeofday(&tv2, NULL);
    auto duration = (double)(tv2.tv_usec - tv1.tv_usec) / 1000000 +
                    (double)(tv2.tv_sec - tv1.tv_sec);
    std::cout << "Recovery Time(s): " << duration << std::endl;
    */

    for (int i = 0; i < thread_num; ++i) {
      rarray[i].workload = not_used_workload;
    }
    if (open_epoch == true) {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Pos_search",
                      &concurr_search);
    } else {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Pos_search",
                      &concurr_search_without_epcoh);
    }

    for (int i = 0; i < thread_num; ++i) {
      rarray[i].begin = operation_num + i * chunk_size;
      rarray[i].end = operation_num + (i + 1) * chunk_size;
    }
    rarray[thread_num - 1].end = 2 * operation_num;
    if (open_epoch == true) {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Neg_search",
                      &concurr_search);
    } else {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Neg_search",
                      &concurr_search_without_epcoh);
    }

    for (int i = 0; i < thread_num; ++i) {
      rarray[i].begin = i * chunk_size;
      rarray[i].end = (i + 1) * chunk_size;
    }
    rarray[thread_num - 1].end = operation_num;

    if (open_epoch == true) {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Delete",
                      &concurr_delete);
    } else {
      GeneralBench<T>(rarray, index, thread_num, operation_num, "Delete",
                      &concurr_delete_without_epoch);
    }
    index->getNumber();
  }

  /*TODO Free the workload memory*/
}

bool check_ratio() {
  int read_portion = (int)(read_ratio * 100);
  int insert_portion = (int)(insert_ratio * 100);
  int delete_portion = (int)(delete_ratio * 100);
  if ((read_portion + insert_portion + delete_portion) != 100) return false;
  return true;
}

int main(int argc, char *argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  initCap = FLAGS_i;
  thread_num = FLAGS_t;
  load_num = FLAGS_n;
  operation_num = FLAGS_p;
  key_type = FLAGS_k;
  index_type = FLAGS_index;
  std::string fixed("fixed");
  operation = FLAGS_op;
  open_epoch = FLAGS_e;
  if (open_epoch == true)
    std::cout << "EPOCH registration in application level" << std::endl;

  read_ratio = FLAGS_r;
  insert_ratio = FLAGS_s;
  delete_ratio = FLAGS_d;
  if (!check_ratio()) {
    std::cout << "The ratio is wrong!" << std::endl;
    return 0;
  }

  if (key_type.compare(fixed) == 0) {
    Run<uint64_t>();
  } else {
    Run<string_key *>();
  }
}