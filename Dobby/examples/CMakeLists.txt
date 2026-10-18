add_executable(socket_example
  main.cc
  socket_example.cc
  )

target_link_libraries(socket_example
  dobby
  logging
  )


add_library(socket_example_lib SHARED
  socket_example.cc
  )

target_link_libraries(socket_example_lib
  dobby
  )

if(DOBBY_WITH_JNI)
  add_library(jni_example_lib SHARED jni_example.cc)
  target_link_libraries(jni_example_lib PRIVATE dobby)
endif()
