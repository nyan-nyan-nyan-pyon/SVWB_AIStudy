cmake_minimum_required(VERSION 3.20)
project(SVWB_AIStudy LANGUAGES CXX)

add_library(svwb_engine STATIC cpp/src/engine.cpp)
target_include_directories(svwb_engine PUBLIC cpp/include)
target_compile_features(svwb_engine PUBLIC cxx_std_20)
set_target_properties(svwb_engine PROPERTIES CXX_EXTENSIONS OFF)

function(svwb_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

if(MSVC)
    target_compile_options(svwb_engine PUBLIC /utf-8)
endif()
svwb_enable_warnings(svwb_engine)

find_package(Threads REQUIRED)
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
add_library(svwb_search STATIC cpp/src/mlp.cpp cpp/src/search.cpp cpp/src/analysis.cpp)
target_link_libraries(svwb_search PUBLIC svwb_engine)
set_target_properties(svwb_search PROPERTIES CXX_EXTENSIONS OFF)
svwb_enable_warnings(svwb_search)

set(SVWB_CARD_DATA "${CMAKE_CURRENT_SOURCE_DIR}/data/cards")
set(SVWB_GENERATED_CARDS "${CMAKE_CURRENT_BINARY_DIR}/generated/wb_card_data.inc")
add_custom_command(OUTPUT "${SVWB_GENERATED_CARDS}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/python/generate_cards.py"
            --data "${SVWB_CARD_DATA}" --output "${SVWB_GENERATED_CARDS}"
    DEPENDS python/generate_cards.py
            data/cards/svwb_rotation_ja.json data/cards/svwb_missing_tokens.json
            data/cards/svwb_crests.json data/cards/svwb_special_forms.json
            data/cards/svwb_supplements_manifest.json data/cards/engine_cards.json
            data/cards/svwb_special_effect_research.json data/cards/phase5b1_cards.json
            data/cards/phase5b2_cards.json
            data/cards/phase5b3_cards.json
    VERBATIM)
add_library(svwb_rules STATIC cpp/src/wb_cards.cpp cpp/src/wb_rules.cpp "${SVWB_GENERATED_CARDS}")
target_include_directories(svwb_rules PUBLIC cpp/include)
target_include_directories(svwb_rules PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(svwb_rules PUBLIC cxx_std_20)
set_target_properties(svwb_rules PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(svwb_rules PUBLIC /utf-8)
endif()
svwb_enable_warnings(svwb_rules)

add_library(svwb_wb_search STATIC cpp/src/wb_search.cpp)
target_link_libraries(svwb_wb_search PUBLIC svwb_search svwb_rules)
set_target_properties(svwb_wb_search PROPERTIES CXX_EXTENSIONS OFF)
svwb_enable_warnings(svwb_wb_search)

add_executable(svwb_ai cpp/src/main.cpp cpp/src/wb_match.cpp)
target_link_libraries(svwb_ai PRIVATE svwb_wb_search Threads::Threads)
set_target_properties(svwb_ai PROPERTIES CXX_EXTENSIONS OFF)
svwb_enable_warnings(svwb_ai)

add_executable(svwb_play_cli cpp/src/play_cli.cpp)
target_link_libraries(svwb_play_cli PRIVATE svwb_wb_search)
set_target_properties(svwb_play_cli PROPERTIES CXX_EXTENSIONS OFF)
svwb_enable_warnings(svwb_play_cli)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_engine cpp/tests/test_engine.cpp)
    target_link_libraries(test_engine PRIVATE svwb_engine)
    set_target_properties(test_engine PROPERTIES CXX_EXTENSIONS OFF)
    svwb_enable_warnings(test_engine)

    add_test(NAME engine_unit COMMAND test_engine)
    add_test(NAME engine_random_10000 COMMAND test_engine --stress)
    set_tests_properties(engine_random_10000 PROPERTIES TIMEOUT 60)

    add_executable(test_search cpp/tests/test_search.cpp)
    target_link_libraries(test_search PRIVATE svwb_search)
    set_target_properties(test_search PROPERTIES CXX_EXTENSIONS OFF)
    svwb_enable_warnings(test_search)
    add_test(NAME search_unit COMMAND test_search)
    set_tests_properties(search_unit PROPERTIES PASS_REGULAR_EXPRESSION "PASS Phase 2 unit suite")

    add_executable(test_wb cpp/tests/test_wb.cpp)
    target_link_libraries(test_wb PRIVATE svwb_rules)
    set_target_properties(test_wb PROPERTIES CXX_EXTENSIONS OFF)
    svwb_enable_warnings(test_wb)
    add_test(NAME wb_rules_unit COMMAND test_wb)
    add_test(NAME wb_mirror_random_10000 COMMAND test_wb --stress)
    add_test(NAME wb_rotation_101_unit COMMAND test_wb --rotation-unit)
    add_test(NAME wb_rotation_mirror_random_10000 COMMAND test_wb --rotation-stress)
    add_test(NAME wb_elf_royal_123_unit COMMAND test_wb --elf-royal-unit)
    add_test(NAME wb_elf_neutral_random_5000 COMMAND test_wb --elf-stress)
    add_test(NAME wb_royal_neutral_random_5000 COMMAND test_wb --royal-stress)
    add_test(NAME wb_dragon_nightmare_119_unit COMMAND test_wb --dragon-nightmare-unit)
    add_test(NAME wb_dragon_neutral_random_5000 COMMAND test_wb --dragon-stress)
    add_test(NAME wb_nightmare_neutral_random_5000 COMMAND test_wb --nightmare-stress)
    set_tests_properties(wb_mirror_random_10000 wb_rotation_mirror_random_10000
        wb_elf_neutral_random_5000 wb_royal_neutral_random_5000
        wb_dragon_neutral_random_5000 wb_nightmare_neutral_random_5000 PROPERTIES TIMEOUT 60)

    if(Python3_Interpreter_FOUND)
        add_test(NAME selfplay_binary_logs COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cpp/tests/check_logs.py"
            --exe "$<TARGET_FILE:svwb_ai>")
        set_tests_properties(selfplay_binary_logs PROPERTIES TIMEOUT 60)
        add_test(NAME analysis_exports COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cpp/tests/check_analysis.py" "$<TARGET_FILE:svwb_ai>")
        set_tests_properties(analysis_exports PROPERTIES TIMEOUT 60)
        add_test(NAME wb_training_exports COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cpp/tests/check_wb_match.py" "$<TARGET_FILE:svwb_ai>")
        set_tests_properties(wb_training_exports PROPERTIES TIMEOUT 60)
        add_test(NAME wb_play_json_lines COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cpp/tests/check_play_cli.py" "$<TARGET_FILE:svwb_play_cli>")
        set_tests_properties(wb_play_json_lines PROPERTIES TIMEOUT 60)
    endif()
endif()
