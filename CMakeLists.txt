cmake_minimum_required(VERSION 3.19)
project(Qwenshiducaiji LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MINGW)
    add_compile_options(-Wa,-mbig-obj)
endif()
include(CTest)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets SerialPort SerialBus PrintSupport Sql)
qt_standard_project_setup()

qt_add_library(SensorRuntime STATIC
    appconfig.h appconfig.cpp
    sensorutils.h sensorutils.cpp serialportpolicy.h
    sensortransport.h sensortransport.cpp
    acquisitioncontroller.h acquisitioncontroller.cpp
    monitoringcontroller.h monitoringcontroller.cpp
    databasemanager.h databasemanager.cpp
    monitorcore.h monitorcore.cpp monitorservice.h monitorservice.cpp
    asynclogger.h asynclogger.cpp historyquery.h historyquery.cpp)
target_include_directories(SensorRuntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(SensorRuntime PUBLIC Qt6::Core Qt6::SerialPort Qt6::SerialBus Qt6::Sql)

qt_add_library(SensorWidgets STATIC
    mainwindow.h mainwindow.cpp mainwindow.ui
    historydialog.h historydialog.cpp alarmdialog.h alarmdialog.cpp
    sensorconfigdialog.h sensorconfigdialog.cpp
    monitordialog.h monitordialog.cpp qcustomplot.h qcustomplot.cpp)
target_link_libraries(SensorWidgets PUBLIC SensorRuntime Qt6::Widgets Qt6::PrintSupport)
qt_add_executable(Qwenshiducaiji WIN32 MACOSX_BUNDLE main.cpp)
target_link_libraries(Qwenshiducaiji PRIVATE SensorWidgets)

install(TARGETS Qwenshiducaiji BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
qt_generate_deploy_app_script(TARGET Qwenshiducaiji OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(QwenshiducaijiTests tests/test_sensorcore.cpp)
    target_link_libraries(QwenshiducaijiTests PRIVATE SensorRuntime Qt6::Test)
    add_test(NAME SensorCoreTests COMMAND QwenshiducaijiTests -o sensor-results.txt,txt)
    qt_add_executable(MonitorBusinessTests tests/test_monitorbusiness.cpp)
    target_link_libraries(MonitorBusinessTests PRIVATE SensorRuntime Qt6::Test)
    add_test(NAME MonitorBusinessTests COMMAND MonitorBusinessTests -o business-results.txt,txt)
    qt_add_executable(MonitorUiTests tests/test_monitorui.cpp)
    target_link_libraries(MonitorUiTests PRIVATE SensorWidgets Qt6::Test)
    add_test(NAME MonitorUiTests COMMAND MonitorUiTests -platform offscreen -o ui-results.txt,txt)
    qt_add_executable(AcquisitionTests tests/test_acquisition.cpp)
    target_link_libraries(AcquisitionTests PRIVATE SensorRuntime Qt6::Test)
    add_test(NAME AcquisitionTests COMMAND AcquisitionTests -o acquisition-results.txt,txt)
    qt_add_executable(WindowLifecycleTests tests/test_windowlifecycle.cpp)
    target_link_libraries(WindowLifecycleTests PRIVATE SensorWidgets Qt6::Test)
    add_test(NAME WindowLifecycleTests COMMAND WindowLifecycleTests -platform offscreen -o lifecycle-results.txt,txt)
    set_tests_properties(SensorCoreTests MonitorBusinessTests MonitorUiTests AcquisitionTests PROPERTIES TIMEOUT 60)
    set_tests_properties(WindowLifecycleTests PROPERTIES TIMEOUT 180)
endif()
