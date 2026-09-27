# Lay out build/test like an fcitx data dir: addon/, inputmethod/, bamboo/.
configure_file(${PROJECT_SOURCE_DIR}/data/vietnamese.cm.dict
               ${CMAKE_CURRENT_BINARY_DIR}/bamboo/vietnamese.cm.dict COPYONLY)
add_custom_target(copy-bamboo-conf
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_BINARY_DIR}/src/bamboo-addon.conf
            ${CMAKE_CURRENT_BINARY_DIR}/addon/bamboo.conf
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_BINARY_DIR}/src/bamboo.conf
            ${CMAKE_CURRENT_BINARY_DIR}/inputmethod/bamboo.conf)
add_dependencies(copy-bamboo-conf bamboo-addon.conf.in-fmt bamboo.conf.in-fmt)

add_executable(testbamboo testbamboo.cpp)
target_link_libraries(testbamboo Fcitx5::Core)
target_compile_definitions(testbamboo PRIVATE
                           "TESTING_BINARY_DIR=\"${PROJECT_BINARY_DIR}\"")
add_dependencies(testbamboo bamboo copy-bamboo-conf)
add_test(NAME testbamboo COMMAND testbamboo)

add_test(NAME testbamboo-go COMMAND go test -count=1 .
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/bamboo)
