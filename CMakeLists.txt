cmake_minimum_required(VERSION 3.19)
project(Qwenshiducaiji LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MINGW)
    add_compile_options(-Wa,-mbig-obj)
endif()
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets SerialPort SerialBus PrintSupport Sql Test)

qt_standard_project_setup()

qt_add_executable(Qwenshiducaiji
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    databasemanager.h
    databasemanager.cpp
    historydialog.h
    historydialog.cpp
    alarmdialog.h
    alarmdialog.cpp
    sensorconfigdialog.h
    sensorconfigdialog.cpp
    sensorutils.h
    sensorutils.cpp
    monitorcore.h monitorcore.cpp
    monitorservice.h monitorservice.cpp
    monitordialog.h monitordialog.cpp
    qcustomplot.h
    qcustomplot.cpp
)

target_link_libraries(Qwenshiducaiji
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::SerialPort
        Qt::SerialBus
        Qt::PrintSupport
        Qt::Sql
)

install(TARGETS Qwenshiducaiji
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET Qwenshiducaiji
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(QwenshiducaijiTests
        tests/test_sensorcore.cpp
        sensorutils.h
        sensorutils.cpp
    )
    target_include_directories(QwenshiducaijiTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(QwenshiducaijiTests PRIVATE Qt::Core Qt::Test)
    add_test(NAME SensorCoreTests COMMAND QwenshiducaijiTests)
    qt_add_executable(MonitorBusinessTests tests/test_monitorbusiness.cpp monitorcore.h monitorcore.cpp monitorservice.h monitorservice.cpp)
    target_include_directories(MonitorBusinessTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(MonitorBusinessTests PRIVATE Qt::Core Qt::Sql Qt::Test)
    add_test(NAME MonitorBusinessTests COMMAND MonitorBusinessTests)
    set_tests_properties(MonitorBusinessTests PROPERTIES TIMEOUT 60)
    qt_add_executable(MonitorUiTests tests/test_monitorui.cpp monitorcore.h monitorcore.cpp monitorservice.h monitorservice.cpp monitordialog.h monitordialog.cpp)
    target_include_directories(MonitorUiTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(MonitorUiTests PRIVATE Qt::Core Qt::Widgets Qt::Sql Qt::Test)
    add_test(NAME MonitorUiTests COMMAND MonitorUiTests -platform offscreen)
    set_tests_properties(MonitorUiTests PROPERTIES TIMEOUT 30)
endif()
