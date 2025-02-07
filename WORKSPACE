http_archive(
  name = "com_google_googletest",
  strip_prefix = "googletest-0fe96607d85cf3a25ac40da369db62bbee2939a5",
  url = "https://github.com/google/googletest/archive/0fe96607d85cf3a25ac40da369db62bbee2939a5.tar.gz",
)

bind(
    name = "gtest",
    actual = "@com_google_googletest//:gtest",
)

http_archive(
  name = "com_google_protobuf",
  strip_prefix = "protobuf-ab8edf1dbe2237b4717869eaab11a2998541ad8d",
  url = "https://github.com/google/protobuf/archive/ab8edf1dbe2237b4717869eaab11a2998541ad8d.tar.gz",
)

bind(
    name = "protobuf",
    actual = "@com_google_protobuf//:protobuf",
)

http_archive(
  name = "com_github_gflags_gflags",
  strip_prefix = "gflags-46f73f88b18aee341538c0dfc22b1710a6abedef",
  url = "https://github.com/gflags/gflags/archive/46f73f88b18aee341538c0dfc22b1710a6abedef.tar.gz",
)

bind(
    name = "gflags",
    actual = "@com_github_gflags_gflags//:gflags",
)

new_http_archive(
  name = "com_github_google_glog",
  build_file = "third-party/glog.BUILD",
  strip_prefix = "glog-a6a166db069520dbbd653c97c2e5b12e08a8bb26",
  url = "https://github.com/google/glog/archive/a6a166db069520dbbd653c97c2e5b12e08a8bb26.tar.gz"
)

bind(
    name = "glog",
    actual = "@com_github_google_glog//:glog",
)

new_http_archive(
  name = "com_github_google_leveldb",
  build_file = "third-party/leveldb.BUILD",
  strip_prefix = "leveldb-a53934a3ae1244679f812d998a4f16f2c7f309a6",
  url = "https://github.com/google/leveldb/archive/a53934a3ae1244679f812d998a4f16f2c7f309a6.tar.gz"
)

# from https://github.com/nelhage/rules_boost
git_repository(
    name = "com_github_nelhage_rules_boost",
    commit = "96ba810e48f4a28b85ee9c922f0b375274a97f98",
    remote = "https://github.com/nelhage/rules_boost",
)

load("@com_github_nelhage_rules_boost//:boost/boost.bzl", "boost_deps")
boost_deps()

# from https://github.com/envoyproxy/envoy/blob/master/bazel/repositories.bzl
new_git_repository(
    name = "com_github_tencent_rapidjson",
    remote= "https://github.com/Tencent/rapidjson.git",
    build_file = "third-party/rapidjson.BUILD",
    tag = "v1.1.0",
)

bind(
    name = "rapidjson",
    actual = "@com_github_tencent_rapidjson//:rapidjson",
)

new_git_repository(
    name = "com_github_facebook_rocksdb",
    remote = "https://github.com/facebook/rocksdb.git",
    #sha256 = "6e8d0844adc37da331844ac4b21ae33ba1f5265d8914c745760d9209a57e9cc9",
    build_file = "third-party/com_github_facebook_rocksdb/BUILD",
    tag = "v5.12.4"
)

bind(
    name = "rocksdb",
    actual = "@com_github_facebook_rocksdb//:rocksdb",
)

# snappy
new_http_archive(
    name = "com_github_google_snappy",
    url = "https://github.com/google/snappy/archive/ed3b7b2.tar.gz",
    strip_prefix = "snappy-ed3b7b242bd24de2ca6750c73f64bee5b7505944",
    sha256 = "88a644b224f54edcd57d01074c2d6fd6858888e915c21344b8622c133c35a337",
    build_file = "third-party/snappy.BUILD",
)

# zlib
new_git_repository(
    name = "com_github_madler_zlib",
    remote = "https://github.com/madler/zlib.git",
    tag = "v1.2.11",
    #sha256 = "629380c90a77b964d896ed37163f5c3a34f6e6d897311f1df2a7016355c45eff",
    build_file = "third-party/zlib.BUILD",
)

bind(
    name = "zlib",
    actual = "@com_github_madler_zlib//:zlib",
)

bind(
    name = "snappy",
    actual = "@com_github_google_snappy//:snappy",
)

bind(
    name = "snappy_config",
    actual = "//third-party/snappy_config:config"
)

git_repository(
    name = "com_github_brpc_braft",
    remote = "https://github.com/brpc/braft.git",
    commit = "a4fd1239631b37a6b08449137f1e1a8fdcd5820d",
)

bind(
    name = "braft",
    actual = "@com_github_brpc_braft//:braft",
)

git_repository(
    name = "com_github_brpc_brpc",
    remote= "https://github.com/apache/incubator-brpc.git",
    tag = "v0.9.0",
)

bind(
    name = "brpc",
    actual = "@com_github_brpc_brpc//:brpc",
)

bind(
    name = "butil",
    actual = "@com_github_brpc_brpc//:butil",
)

bind(
    name = "bthread",
    actual = "@com_github_brpc_brpc//:bthread",
)

bind(
    name = "bvar",
    actual = "@com_github_brpc_brpc//:bvar",
)

bind(
    name = "json2pb",
    actual = "@com_github_brpc_brpc//:json2pb",
)
