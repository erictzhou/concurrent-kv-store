#ifndef KV_STORE_COMMON_VERSION_H_
#define KV_STORE_COMMON_VERSION_H_

#ifndef CONCURRENT_KV_STORE_VERSION
#define CONCURRENT_KV_STORE_VERSION "unknown"
#endif

namespace kv {
namespace common {

inline constexpr const char* kProjectVersion = CONCURRENT_KV_STORE_VERSION;
inline constexpr const char* kConcurrencyModel =
    "64 shards: per-shard reader/writer locks; independent keys proceed concurrently";

}  // namespace common
}  // namespace kv

#endif  // KV_STORE_COMMON_VERSION_H_
