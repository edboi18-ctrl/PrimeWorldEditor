# Tests for pwe_core (no Qt). Run with `ctest` in the build folder.
add_executable(pwe_collision_cooker_tests CollisionCookerTests.cpp)
target_compile_features(pwe_collision_cooker_tests PRIVATE cxx_std_23)
target_link_libraries(pwe_collision_cooker_tests PRIVATE pwe_core)
add_test(NAME collision_cooker COMMAND pwe_collision_cooker_tests)
