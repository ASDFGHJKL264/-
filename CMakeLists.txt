cmake_minimum_required(VERSION 3.19)
project(Qwenshiducaiji LANGUAGES CXX)
if(MINGW)
    add_compile_options(-Wa,-mbig-obj)
endif()
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets SerialPort SerialBus PrintSupport Sql)

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
