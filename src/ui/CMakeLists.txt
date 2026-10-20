# slowmail: Qt Quick UI. Keep to APIs available in Qt 6.4 (host) and 6.8 (reMarkable SDK).
cmake_minimum_required(VERSION 3.22)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
qt_standard_project_setup()

set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(slowmail
    main.cpp
    triage_controller.cpp
    ../cli/app_context.cpp
)

qt_add_qml_module(slowmail
    URI SlowMail
    VERSION 1.0
    RESOURCE_PREFIX /
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/SMButton.qml
        qml/PaperclipIcon.qml
        qml/ErrorBanner.qml
        qml/QueuePage.qml
        qml/MessagePage.qml
        qml/ActivityPage.qml
        qml/SignInPage.qml
)

target_include_directories(slowmail PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../cli)
target_link_libraries(slowmail PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    slowmail_core slowmail_storage slowmail_graph slowmail_platform
)
# Warnings only for our own sources (qmlcachegen output is noisy under -Wshadow).
set_source_files_properties(main.cpp triage_controller.cpp ../cli/app_context.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wno-sign-conversion")
