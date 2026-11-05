#pragma once

#include <filesystem>

// 呼び出し側がパス・設定の準備と実行スレッドを管理する共通初期化。
// wx 起動では CemuApp::OnInit、LLE では従来の呼び出し位置を維持する。
// UI の生成やイベントループの開始、多重初期化・再起動は担当しない。
void CemuCommonInit(const std::filesystem::path& workingDirectory = {});
