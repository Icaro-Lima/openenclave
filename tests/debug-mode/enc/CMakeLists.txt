# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

add_enclave(TARGET no_debug_enc CONF sign.conf SOURCES enc.c props.c)

add_enclave(TARGET debug_enc CONF sign-debug.conf SOURCES enc.c props-debug.c)

target_link_libraries(no_debug_enc oeenclave)
target_link_libraries(debug_enc oeenclave)
