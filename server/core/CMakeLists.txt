add_library(maxscale-common SHARED
  admin.cc
  adminusers.cc
  alloc.cc
  authenticator.cc
  backend.cc
  buffer.cc
  config.cc
  config_runtime.cc
  dcb.cc
  encryption.cc
  event.cc
  externcmd.cc
  filter.cc
  hint.cc
  housekeeper.cc
  httprequest.cc
  httpresponse.cc
  json_api.cc
  listener.cc
  load_utils.cc
  log.cc
  mariadb.cc
  maxscale_pcre2.cc
  misc.cc
  modulecmd.cc
  modutil.cc
  monitor.cc
  mysql_binlog.cc
  mysql_utils.cc
  paths.cc
  poll.cc
  queryclassifier.cc
  query_classifier.cc
  random.cc
  resource.cc
  response_stat.cc
  resultset.cc
  router.cc
  routingworker.cc
  secrets.cc
  server.cc
  service.cc
  session.cc
  session_command.cc
  ssl.cc
  users.cc
  utils.cc
  session_stats.cc
  )

target_link_libraries(maxscale-common
  maxbase
  ${MARIADB_CONNECTOR_LIBRARIES}
  ${LZMA_LINK_FLAGS}
  ${PCRE2_LIBRARIES}
  ${JANSSON_LIBRARIES}
  ssl
  pthread
  crypt
  crypto
  inih
  z
  rt
  m
  sqlite3
  stdc++
  gnutls
  gcrypt
  ${MICROHTTPD_LIBRARIES}
)

if(WITH_ASAN AND ASAN_FOUND)
  target_link_libraries(maxscale-common ${ASAN_LIBRARIES})
endif()

find_library(HAVE_LIBDL NAMES dl)
if (HAVE_LIBDL)
  # libdl just exposes libc functionality on most systems.  This means that if
  # we can't find it, it's likely that libc already exposes the symbols.
  target_link_libraries(maxscale-common dl)
endif()

# Using initial-exec instead of the default global-dynamic tls-model
# reduces the cost of using thread-local variables in dynamic libraries.
target_compile_options(maxscale-common PRIVATE "-ftls-model=initial-exec")

add_dependencies(maxscale-common pcre2 connector-c libmicrohttpd jansson maxbase)
set_target_properties(maxscale-common PROPERTIES VERSION "1.0.0")
install_module(maxscale-common core)

add_executable(maxscale gateway.cc)
add_dependencies(maxscale pcre2)

if(WITH_JEMALLOC)
  target_link_libraries(maxscale ${JEMALLOC_LIBRARIES})
elseif(WITH_TCMALLOC)
  target_link_libraries(maxscale ${TCMALLOC_LIBRARIES})
endif()

target_link_libraries(maxscale maxscale-common)
install_executable(maxscale core)

add_executable(maxkeys maxkeys.c)
target_link_libraries(maxkeys maxscale-common)
install_executable(maxkeys core)

add_executable(maxpasswd maxpasswd.c)
target_link_libraries(maxpasswd maxscale-common)
install_executable(maxpasswd core)

if(BUILD_TESTS)
  add_subdirectory(test)
endif()
