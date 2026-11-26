/*******************************************************************************
The content of this file includes portions of the AUDIOKINETIC Wwise Technology
released in source code form as part of the SDK installer package.

Commercial License Usage

Licensees holding valid commercial licenses to the AUDIOKINETIC Wwise Technology
may use this file in accordance with the end user license agreement provided
with the software or, alternatively, in accordance with the terms contained in a
written agreement between you and Audiokinetic Inc.

Apache License Usage

Alternatively, this file may be used under the Apache License, Version 2.0 (the
"Apache License"); you may not use this file except in compliance with the
Apache License. You may obtain a copy of the Apache License at
http://www.apache.org/licenses/LICENSE-2.0.

Unless required by applicable law or agreed to in writing, software distributed
under the Apache License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES
OR CONDITIONS OF ANY KIND, either express or implied. See the Apache License for
the specific language governing permissions and limitations under the License.

  Copyright (c) 2026 Audiokinetic Inc.
*******************************************************************************/

#include "OKConvolutionPluginGUI.h"
#include "../../OKConvolutionConfig.h"
#include <commdlg.h> // For GetOpenFileNameW
#include "resource.h"

namespace
{
    static_assert(OKConvolutionConfig::MaxIRSlots == 4u,
        "Update Win32 dialog rows in OkConvolution.rc when MaxIRSlots changes.");
    static_assert(IDC_BUTTON_BROWSE_IR_3 - IDC_BUTTON_BROWSE_IR_0 == OKConvolutionConfig::MaxIRSlots - 1u,
        "Browse button control IDs must be contiguous.");
    static_assert(IDC_BUTTON_CLEAR_IR_3 - IDC_BUTTON_CLEAR_IR_0 == OKConvolutionConfig::MaxIRSlots - 1u,
        "Clear button control IDs must be contiguous.");
    static_assert(IDC_STATIC_IR_NAME_3 - IDC_STATIC_IR_NAME_0 == OKConvolutionConfig::MaxIRSlots - 1u,
        "Filename static label control IDs must be contiguous.");
}

AK_WWISE_PLUGIN_GUI_WINDOWS_BEGIN_POPULATE_TABLE(OKConvolutionProp)
AK_WWISE_PLUGIN_GUI_WINDOWS_END_POPULATE_TABLE()

bool OKConvolutionPluginGUI::GetDialog(AK::Wwise::Plugin::eDialog in_eDialog, uint32_t& out_uiDialogID,
    AK::Wwise::Plugin::PopulateTableItem*& out_pTable) const
{
    if (in_eDialog == AK::Wwise::Plugin::SettingsDialog)
    {
        out_uiDialogID = IDD_OKCONVOLUTION_BIG;
        out_pTable = OKConvolutionProp;
        return true;
    }
    return false;
}

bool OKConvolutionPluginGUI::WindowProc(AK::Wwise::Plugin::eDialog in_eDialog, HWND in_hWnd, uint32_t in_message,
    WPARAM in_wParam, LPARAM in_lParam, LRESULT& out_lResult)
{
    if (in_eDialog != AK::Wwise::Plugin::SettingsDialog)
        return false;

    switch (in_message)
    {
    case WM_INITDIALOG:
        // Track the settings dialog HWND so external media updates (such as Undo/Redo)
        // can repaint the slot filename labels, while returning false so Wwise still
        // initializes its bound SuperRange property controls.
        m_hWndSettings = in_hWnd;
        RefreshSlotLabels(in_hWnd);
        return false;

    case WM_DESTROY:
        if (m_hWndSettings == in_hWnd)
            m_hWndSettings = nullptr;
        return false;

    case WM_COMMAND:
        if (HIWORD(in_wParam) == BN_CLICKED)
        {
            const unsigned int uControlId = static_cast<unsigned int>(LOWORD(in_wParam));

            if (uControlId >= IDC_BUTTON_BROWSE_IR_0
                && uControlId < IDC_BUTTON_BROWSE_IR_0 + OKConvolutionConfig::MaxIRSlots)
            {
                BrowseAndAssignSlot(in_hWnd, uControlId - IDC_BUTTON_BROWSE_IR_0);
                out_lResult = 0;
                return true;
            }

            if (uControlId >= IDC_BUTTON_CLEAR_IR_0
                && uControlId < IDC_BUTTON_CLEAR_IR_0 + OKConvolutionConfig::MaxIRSlots)
            {
                ClearSlot(in_hWnd, uControlId - IDC_BUTTON_CLEAR_IR_0);
                out_lResult = 0;
                return true;
            }
        }
        break;

    default:
        break;
    }

    return false;
}

void OKConvolutionPluginGUI::NotifyPluginMediaChanged()
{
    // Keep all 4 slot filename labels and Clear button states in sync whenever
    // Wwise adds, replaces, removes, or undoes/redoes plugin media changes.
    if (m_hWndSettings != nullptr)
        RefreshSlotLabels(m_hWndSettings);
}

void OKConvolutionPluginGUI::BrowseAndAssignSlot(HWND in_hWnd, unsigned int in_uSlotIndex)
{
    if (in_uSlotIndex >= OKConvolutionConfig::MaxIRSlots)
        return;

    wchar_t szFilePath[MAX_PATH] = L"";

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = in_hWnd;
    ofn.lpstrFilter = L"WAV Files (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = szFilePath;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn))
        return;

    // Wwise ObjectMedia requires media indices to be created sequentially: calling
    // SetMediaSource() past the current count silently appends at GetMediaSourceCount()
    // instead of the requested index. Insert empty placeholder entries for any missing
    // preceding slots so out-of-order slot selection lands at the exact target index.
    const unsigned int uCurrentCount = m_objectMedia.GetMediaSourceCount();
    for (unsigned int i = uCurrentCount; i < in_uSlotIndex; ++i)
    {
        m_objectMedia.SetMediaSource(nullptr, i, false);
    }

    m_objectMedia.SetMediaSource(szFilePath, in_uSlotIndex, true);
    RefreshSlotLabels(in_hWnd);
}

void OKConvolutionPluginGUI::ClearSlot(HWND in_hWnd, unsigned int in_uSlotIndex)
{
    if (in_uSlotIndex >= OKConvolutionConfig::MaxIRSlots)
        return;

    // RemoveMediaSource empties the target index without renumbering higher slots,
    // preserving the slot assignments of all other loaded IRs.
    if (in_uSlotIndex < m_objectMedia.GetMediaSourceCount())
        m_objectMedia.RemoveMediaSource(in_uSlotIndex);

    RefreshSlotLabels(in_hWnd);
}

void OKConvolutionPluginGUI::RefreshSlotLabels(HWND in_hWnd) const
{
    if (!in_hWnd || !IsWindow(in_hWnd))
        return;

    const unsigned int uMediaCount = m_objectMedia.GetMediaSourceCount();
    for (unsigned int slot = 0; slot < OKConvolutionConfig::MaxIRSlots; ++slot)
    {
        wchar_t szFileName[MAX_PATH] = L"";
        bool bHasMedia = false;

        if (slot < uMediaCount)
        {
            const unsigned int uWritten = m_objectMedia.GetMediaSourceFileName(szFileName, MAX_PATH, slot);
            bHasMedia = (uWritten > 0 && szFileName[0] != L'\0');
        }

        SetDlgItemTextW(in_hWnd, IDC_STATIC_IR_NAME_0 + slot, bHasMedia ? szFileName : L"(Empty)");

        if (HWND hClearButton = GetDlgItem(in_hWnd, IDC_BUTTON_CLEAR_IR_0 + slot))
            EnableWindow(hClearButton, bHasMedia ? TRUE : FALSE);
    }
}

AK_ADD_PLUGIN_CLASS_TO_CONTAINER(
    OKConvolution,
    OKConvolutionPluginGUI,
    OKConvolutionFX
);