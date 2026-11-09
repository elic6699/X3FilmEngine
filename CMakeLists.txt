cmake_minimum_required(VERSION 3.20)
project(SpektraLab LANGUAGES C CXX)

include(CTest)

# The host protocol is built on C++ exceptions (HostError / std::system_error).
# MSVC's default CMAKE_CXX_FLAGS carries /EHsc, but a caller that overrides that
# variable (for example with /utf-8 for the UTF-8 comments) silently drops it.
# Without /EHsc, MSVC still catches the throw but does NOT unwind the stack,
# so std::lock_guard objects never unlock: a cancelled render leaves cancel_mutex
# owned by the worker thread and the next render fails with EDEADLK.
# Re-assert it as a compile option so no flag override can remove it.
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
endif()

add_subdirectory(engine)
