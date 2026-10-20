# slowmail: Qt Quick UI. Keep to APIs available in Qt 6.4 (host) and 6.8 (reMarkable SDK).
cmake_minimum_required(VERSION 3.22)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
qt_standard_project_setup()

set(SLOWMAIL_QML_FILES
    Main.qml Theme.qml SMButton.qml PaperclipIcon.qml ErrorBanner.qml
    QueuePage.qml MessagePage.qml ActivityPage.qml SignInPage.qml)
set(SLOWMAIL_QML_PATHS)
foreach(f IN LISTS SLOWMAIL_QML_FILES)
    # Sources live in qml/, but are served at the module root so the module qmldir (and its singleton) apply.
    set_source_files_properties(qml/${f} PROPERTIES QT_RESOURCE_ALIAS ${f})
    list(APPEND SLOWMAIL_QML_PATHS qml/${f})
endforeach()
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
    QML_FILES ${SLOWMAIL_QML_PATHS}
)

target_include_directories(slowmail PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../cli)
target_link_libraries(slowmail PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    slowmail_core slowmail_storage slowmail_graph slowmail_platform
)
# Warnings only for our own sources (qmlcachegen output is noisy under -Wshadow).
set_source_files_properties(main.cpp triage_controller.cpp ../cli/app_context.cpp PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wno-sign-conversion")
