cmake_minimum_required(VERSION 3.16)
project(CoFinance VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局约定：C++17、导出 compile_commands.json（clangd/编辑器依赖它）
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

option(COFINANCE_BUILD_TESTS "构建单元测试" ON)
option(COFINANCE_SANITIZE   "开启 ASan + UBSan（Debug 建议开启）" OFF)

# ---------------------------------------------------------------------------
# 内存/UB 检测：Debug 下强烈建议 COFINANCE_SANITIZE=ON
#
# 必须放在所有 target 定义之前：add_compile_options / add_link_options 是
# 目录级设置，只对「之后创建」的 target 生效。曾经用 foreach 逐个 target 加，
# 结果漏掉了 cofinance_tests —— 静态库里被插桩的目标文件引用了 __asan_* 符号，
# 而链接方没链 ASan 运行时，于是报一堆 undefined reference 到 __asan_report_load8。
# 教训：插桩要成对出现（编译期插桩 + 链接期运行时），别用枚举 target 的方式维护。
# ---------------------------------------------------------------------------
if(COFINANCE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "已开启 ASan + UBSan（对全部 target 生效）")
endif()

# ---------------------------------------------------------------------------
# 依赖：全部 REQUIRED —— 环境不完整就让 configure 阶段直接失败，
# 而不是等到链接期才报一堆看不懂的 undefined reference
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)

# MySQL 官方包不提供 CMake config 文件，手工查找
find_path(MYSQL_INCLUDE_DIR NAMES mysql.h PATH_SUFFIXES mysql)
find_library(MYSQL_LIBRARY NAMES mysqlclient mysqlclient_r)
if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
    message(FATAL_ERROR "找不到 MySQL 客户端库，请安装 libmysqlclient-dev")
endif()
add_library(MySQL::client UNKNOWN IMPORTED)
set_target_properties(MySQL::client PROPERTIES
    IMPORTED_LOCATION "${MYSQL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MYSQL_INCLUDE_DIR}")

# ---------------------------------------------------------------------------
# 核心静态库：除 main.cpp 外的全部实现，便于单元测试直接链接
# 注意这里不用 file(GLOB)：GLOB 在新增文件时不会触发 cmake 重新配置
# ---------------------------------------------------------------------------
set(COFINANCE_CORE_SOURCES
    src/util/Common.cpp
    src/util/Config.cpp
    src/util/Logger.cpp
    src/util/Crypto.cpp
    src/net/Socket.cpp
    src/net/Epoller.cpp
    src/net/TimerQueue.cpp
    src/net/EventLoop.cpp
    src/net/TcpConnection.cpp
    src/net/TcpServer.cpp
    src/http/HttpTypes.cpp
    src/http/HttpParser.cpp
    src/http/Router.cpp
    src/http/StaticFileHandler.cpp
    src/http/HttpServer.cpp
    src/db/MySqlConnection.cpp
    src/db/PreparedStatement.cpp
    src/db/ConnectionPool.cpp
    src/db/TransactionGuard.cpp
    src/db/UserDao.cpp
    src/db/SessionDao.cpp
    src/db/GroupDao.cpp
    src/api/ApiSupport.cpp
    src/service/UserService.cpp
    src/service/GroupService.cpp
    src/api/UserApi.cpp
    src/api/GroupApi.cpp
)

add_library(cofinance_core STATIC ${COFINANCE_CORE_SOURCES})
target_include_directories(cofinance_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(cofinance_core
    PUBLIC  Threads::Threads
    PRIVATE OpenSSL::SSL OpenSSL::Crypto CURL::libcurl nlohmann_json::nlohmann_json MySQL::client)

# 编译告警：按 target 设置，而不是污染全局 CMAKE_CXX_FLAGS
function(cofinance_enable_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror)
endfunction()
cofinance_enable_warnings(cofinance_core)

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------
add_executable(cofinance src/main.cpp)
target_link_libraries(cofinance PRIVATE cofinance_core)
target_compile_definitions(cofinance PRIVATE
    COFINANCE_VERSION="${PROJECT_VERSION}"
    COFINANCE_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
cofinance_enable_warnings(cofinance)

# ---------------------------------------------------------------------------
# 单元测试：保持零第三方依赖的极简断言，避免为测试框架额外引入联网依赖
# ---------------------------------------------------------------------------
if(COFINANCE_BUILD_TESTS)
    add_executable(cofinance_tests tests/unit/test_main.cpp)
    target_link_libraries(cofinance_tests PRIVATE cofinance_core)
    cofinance_enable_warnings(cofinance_tests)

    add_executable(cofinance_net_tests tests/unit/test_net.cpp)
    target_link_libraries(cofinance_net_tests PRIVATE cofinance_core)
    cofinance_enable_warnings(cofinance_net_tests)

    add_executable(cofinance_http_tests tests/unit/test_http.cpp)
    target_link_libraries(cofinance_http_tests PRIVATE cofinance_core)
    cofinance_enable_warnings(cofinance_http_tests)

    add_executable(cofinance_db_tests tests/unit/test_db.cpp)
    target_link_libraries(cofinance_db_tests PRIVATE cofinance_core)
    cofinance_enable_warnings(cofinance_db_tests)

    add_executable(cofinance_auth_tests tests/unit/test_auth.cpp)
    target_link_libraries(cofinance_auth_tests PRIVATE cofinance_core)
    cofinance_enable_warnings(cofinance_auth_tests)

    add_executable(cofinance_group_tests tests/unit/test_group.cpp)
    target_link_libraries(cofinance_group_tests PRIVATE cofinance_core)
    cofinance_enable_warnings(cofinance_group_tests)

    add_executable(cofinance_echo_server tests/integration/echo_server_main.cpp)
    target_link_libraries(cofinance_echo_server PRIVATE cofinance_core)
    cofinance_enable_warnings(cofinance_echo_server)

    enable_testing()
    add_test(NAME unit COMMAND cofinance_tests)
    add_test(NAME net COMMAND cofinance_net_tests)
    add_test(NAME http COMMAND cofinance_http_tests)
    add_test(NAME db COMMAND cofinance_db_tests
        ${CMAKE_SOURCE_DIR}/config/server.conf ${CMAKE_SOURCE_DIR}/sql/schema.sql)
    add_test(NAME auth COMMAND cofinance_auth_tests
        ${CMAKE_SOURCE_DIR}/config/server.conf ${CMAKE_SOURCE_DIR}/sql/schema.sql)
    add_test(NAME group COMMAND cofinance_group_tests
        ${CMAKE_SOURCE_DIR}/config/server.conf ${CMAKE_SOURCE_DIR}/sql/schema.sql)
endif()

message(STATUS "构建类型: ${CMAKE_BUILD_TYPE} | 测试: ${COFINANCE_BUILD_TESTS} | 净化器: ${COFINANCE_SANITIZE}")
