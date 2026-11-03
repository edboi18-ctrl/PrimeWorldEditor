# Tests for pwe_core (no Qt). Run with `ctest` in the build folder.
add_executable(pwe_collision_cooker_tests CollisionCookerTests.cpp)
target_compile_features(pwe_collision_cooker_tests PRIVATE cxx_std_23)
target_link_libraries(pwe_collision_cooker_tests PRIVATE pwe_core)
add_test(NAME collision_cooker COMMAND pwe_collision_cooker_tests)

add_executable(pwe_world_geometry_cooker_tests WorldGeometryCookerTests.cpp)
target_compile_features(pwe_world_geometry_cooker_tests PRIVATE cxx_std_23)
target_link_libraries(pwe_world_geometry_cooker_tests PRIVATE pwe_core)
add_test(NAME world_geometry_cooker COMMAND pwe_world_geometry_cooker_tests)

add_executable(pwe_area_cooker_tests AreaCookerTests.cpp)
target_compile_features(pwe_area_cooker_tests PRIVATE cxx_std_23)
target_link_libraries(pwe_area_cooker_tests PRIVATE pwe_core)
add_test(NAME area_cooker COMMAND pwe_area_cooker_tests)

# The Blender add-on's Blender-free core (tools/blender): its own tests, then the room it builds read
# through this repo's C++ engine readers and loaders. Needs Python 3 with numpy (Blender ships both).
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import numpy" RESULT_VARIABLE PWE_HAVE_NUMPY OUTPUT_QUIET ERROR_QUIET)
    if (PWE_HAVE_NUMPY EQUAL 0)
        set(PWE_ADDON_DUMP ${CMAKE_CURRENT_BINARY_DIR}/blender_addon)
        add_test(NAME blender_addon_core
                 COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/blender/tests/test_core.py --dump ${PWE_ADDON_DUMP})
        add_test(NAME blender_addon_room_check
                 COMMAND pwe_world_geometry_cooker_tests --check-mrea ${PWE_ADDON_DUMP}/addon_room.MREA)
        set_tests_properties(blender_addon_core PROPERTIES FIXTURES_SETUP blender_addon_room)
        set_tests_properties(blender_addon_room_check PROPERTIES FIXTURES_REQUIRED blender_addon_room)
    else()
        message(STATUS "numpy not found: the Blender add-on tests are skipped")
    endif()
endif()
