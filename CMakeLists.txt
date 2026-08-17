cmake_minimum_required(VERSION 3.19)
project(Qwenshiducaiji LANGUAGES CXX)
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
endif()
