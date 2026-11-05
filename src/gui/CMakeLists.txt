add_library(CemuGui INTERFACE)
target_include_directories(CemuGui INTERFACE "interface")

if(ENABLE_WXWIDGETS)
    add_subdirectory(wxgui)
    # 最終実行ファイルごとに backend を選び、WindowSystem の重複定義を避ける。
    target_link_libraries(CemuGui INTERFACE "$<$<NOT:$<BOOL:$<TARGET_PROPERTY:CEMU_NATIVE_HOST>>>:CemuWxGui>")
endif()
