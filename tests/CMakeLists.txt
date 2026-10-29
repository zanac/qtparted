find_package(Qt6 REQUIRED COMPONENTS Test)

# Pure model tests: no devices touched.
add_executable(tst_model tst_model.cpp)
target_link_libraries(tst_model PRIVATE qtparted-core Qt6::Test)
add_test(NAME model COMMAND tst_model)

# End to end tests on disk image files and, when running as root with loop device
# support, on loop devices (creates, formats and deletes real partitions).
add_executable(tst_disk tst_disk.cpp)
target_link_libraries(tst_disk PRIVATE qtparted-core Qt6::Test)
add_test(NAME disk COMMAND tst_disk)

# Resize, move, copy, check, label and UUID with data integrity checks (root, loop devices).
add_executable(tst_f2 tst_f2.cpp)
target_link_libraries(tst_f2 PRIVATE qtparted-core Qt6::Test)
add_test(NAME f2 COMMAND tst_f2)
