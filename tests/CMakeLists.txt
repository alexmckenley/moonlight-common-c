cmake_minimum_required(VERSION 3.16)
project(rtp_video_queue_tests C)

enable_testing()
find_package(OpenSSL REQUIRED)
add_executable(rtp_video_queue_tests
    test_rtp_video_queue.c
    ../src/RtpVideoQueue.c
    ../nanors/rs.c
    ../nanors/deps/obl/oblas_common.c
    ../nanors/deps/obl/oblas_lite.c)
target_compile_features(rtp_video_queue_tests PRIVATE c_std_11)
target_compile_definitions(rtp_video_queue_tests PRIVATE HAS_SOCKLEN_T)
target_compile_definitions(rtp_video_queue_tests PRIVATE $<$<CONFIG:Debug>:LC_DEBUG>)
target_include_directories(rtp_video_queue_tests PRIVATE
    ../src ../enet/include ../nanors ../nanors/deps ../nanors/deps/obl)
target_link_libraries(rtp_video_queue_tests PRIVATE OpenSSL::Crypto)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rtp_video_queue_tests PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
endif()
add_test(NAME rtp_video_queue COMMAND rtp_video_queue_tests)
