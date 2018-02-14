# create a binary for each testcase listed in ../tests.supported

include(add_enclave_executable)
include(${CMAKE_CURRENT_LIST_DIR}/../get_testcase_name.cmake)

# helper lib to contain file needed by some tests
add_library(libctest-support
    mtest.c
    rand.c
    tls_align_dso.c
    )
target_link_libraries(libctest-support PUBLIC oelibc)

function(add_libc_test_enc NAME CFILE)
    add_enclave_executable(libctest-${NAME} sign.conf private.pem
        enc.c
        main.c
        )

    target_include_directories(libctest-${NAME} PRIVATE 
        ..
        ${PROJECT_SOURCE_DIR}/3rdparty/musl/libc-test/src/common
        )
    target_compile_options(libctest-${NAME} PRIVATE
        -Wno-error=missing-braces
        -Wno-error=overflow
        -Wno-error=unused-value
        -Wno-error=unused-variable
        )
    target_compile_definitions(libctest-${NAME} PRIVATE -D__TEST__="${CFILE}")
    target_link_libraries(libctest-${NAME} libctest-support)
endfunction(add_libc_test_enc)

file(STRINGS "../tests.supported" alltests)
foreach(testcase ${alltests})
    get_testcase_name(${testcase} name)
    add_libc_test_enc("${name}" "${testcase}")
endforeach(testcase)