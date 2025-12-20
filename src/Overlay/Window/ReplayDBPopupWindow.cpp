#include "ReplayDBPopupWindow.h"

#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/utils.h"

#include "Core/info.h"
#include "Overlay/imgui_utils.h"
#include <cstdlib>
#include <string>


void ReplayDBPopupWindow::Draw()
{
    ImVec4 black = ImVec4(0.060, 0.060, 0.060, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, black);
    const char* popupTitle = Messages.Enable_Disable_Automatic_Replay_Uploads();
    ImGui::OpenPopup(popupTitle);

    const ImVec2 buttonSize = ImVec2(120, 23);
    ImGui::SetNextWindowPosCenter(ImGuiCond_Appearing);

    ImGui::BeginPopupModal(popupTitle, NULL, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::TextWrapped(Messages.IM_BOBLEIS_can_automatically_upload_your_replays_to_a_public_central_repository_This_feature_allows_Easy_sharing_Facilitates_sharing_of_replays_between_players_Community_sharing_Contribute_to_a_collection_of_matches_for_learning_and_analysis_Cataloging_Easily_search_by_characters_for_matchups_Storage_Keep_your_replays_backed_up_automatically_after_the_100_limit_You_can_access_the_database_using_the_Replay_Database_button_on_the_IM_main_menu_If_you_disable_it_your_replays_won_t_be_uploaded_even_if_matched_with_someone_with_it_enabled_If_you_wish_to_turn_it_off_on_again_you_can_access_this_menu_using_the_button_on_the_IM_main_menu_or_using_settings_ini_This_has_no_impact_on_performance_or_stability_Would_you_like_to_leave_automatic_replay_uploads_on_or_turn_it_off());
        ImGui::Separator();
        ImGui::AlignItemHorizontalCenter(buttonSize.x);
        if (ImGui::Button((std::string(Messages.ON()) + "##dbpopup").c_str(), buttonSize)) {
            Settings::changeSetting("UploadReplayData", std::to_string(1));
            Settings::settingsIni.uploadReplayData = 1;
            g_modVals.uploadReplayData = 1;
            ImGui::CloseCurrentPopup();
            Close();
        }
        //ImGui::SameLine();
        ImGui::AlignItemHorizontalCenter(buttonSize.x);
        if (ImGui::Button((std::string(Messages.OFF()) + "##dbpopup").c_str(), buttonSize)) {
            Settings::changeSetting("UploadReplayData", std::to_string(0));
            Settings::settingsIni.uploadReplayData = 0;
            g_modVals.uploadReplayData = 0;
            ImGui::CloseCurrentPopup();
            Close();
        }
        ImGui::EndPopup();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();


}
