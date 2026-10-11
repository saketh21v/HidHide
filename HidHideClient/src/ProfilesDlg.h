// (c) Eric Korff de Gidts
// SPDX-License-Identifier: MIT
// ProfilesDlg.h
#pragma once
#include <functional>
#include "IDropTarget.h"
#include "FilterDriverProxy.h"

class CHidHideClientDlg;

// A small modal dialog that prompts the user for a single line of input
class CInputDlg : public CDialogEx
{
    DECLARE_DYNAMIC(CInputDlg)

public:

    CInputDlg() noexcept = delete;
    CInputDlg(_In_ CInputDlg const& rhs) = delete;
    CInputDlg(_In_ CInputDlg&& rhs) noexcept = delete;
    CInputDlg& operator=(_In_ CInputDlg const& rhs) = delete;
    CInputDlg& operator=(_In_ CInputDlg&& rhs) noexcept = delete;

    // Construct the dialog with the title, guidance text, initial value, a validator and an error message
    // The validator returns TRUE when the entered value is acceptable; on rejection the error message is shown
    CInputDlg(_In_ std::wstring const& title, _In_ std::wstring const& guidance, _In_ std::wstring const& value, _In_ std::function<bool(std::wstring const&)> validator, _In_ std::wstring const& errorMessage, _In_opt_ CWnd* pParent);

    // The value entered (valid after the dialog returned IDOK)
    std::wstring const& Value() const noexcept;

private:

    // Dialog Data
#ifdef AFX_DESIGN_TIME
    enum { IDD = IDD_DIALOG_PROFILE_NAME };
#endif

    void DoDataExchange(_In_ CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    void OnOK() override;

    std::wstring const                       m_Title;
    std::wstring const                       m_Guidance;
    std::wstring                             m_Value;
    std::function<bool(std::wstring const&)> m_Validator;
    std::wstring const                       m_ErrorMessage;

    // Controls
    CStatic m_GuidanceControl;
    CEdit   m_Edit;
};

// The application profiles tab
class CProfilesDlg : public CDialogEx, public HidHide::IDropTarget
{
    DECLARE_DYNAMIC(CProfilesDlg)

public:

    CProfilesDlg() noexcept = delete;
    CProfilesDlg(_In_ CProfilesDlg const& rhs) = delete;
    CProfilesDlg(_In_ CProfilesDlg&& rhs) noexcept = delete;
    CProfilesDlg& operator=(_In_ CProfilesDlg const& rhs) = delete;
    CProfilesDlg& operator=(_In_ CProfilesDlg&& rhs) noexcept = delete;

    CProfilesDlg(_In_ CHidHideClientDlg& hidHideClientDlg, _In_opt_ CWnd* pParent);
    virtual ~CProfilesDlg();

    // Called when the cursor first enters the window
    DROPEFFECT OnDragEnter(_In_ CWnd* pWnd, _In_ COleDataObject* pDataObject, _In_ DWORD dwKeyState, _In_ CPoint point) override;

    // Called repeatedly when the cursor is dragged over the window
    DROPEFFECT OnDragOver(_In_ CWnd* pWnd, _In_ COleDataObject* pDataObject, _In_ DWORD dwKeyState, _In_ CPoint point) override;

    // Called when data is dropped into the window, initial handler
    DROPEFFECT OnDropEx(_In_ CWnd* pWnd, _In_ COleDataObject* pDataObject, _In_ DROPEFFECT dropDefault, _In_ DROPEFFECT dropList, _In_ CPoint point) override;

private:

    // Dialog Data
#ifdef AFX_DESIGN_TIME
    enum { IDD = IDD_DIALOG_PROFILES };
#endif

    void DoDataExchange(_In_ CDataExchange* pDX) override;
    BOOL OnInitDialog() override;

    // Post a request to refresh the profiles and the detail of the selected one
    void Refresh();

    // Fill the applications and devices lists for the currently selected profile
    void RefreshDetail();

    // The name of the currently selected profile (empty when none is selected)
    std::wstring SelectedProfile() const;

    // Rewrite the selected profile from the list box contents
    void StoreSelectedProfileFromLists();

    // Is the mouse pointer in the applications list control?
    bool MousePointerAtApplications(_In_ CPoint point);

    // User Message on refresh
    LRESULT OnUserMessageRefresh(_In_ WPARAM wParam, _In_ LPARAM lParam);

    // The shared filter driver proxy
    HidHide::FilterDriverProxy& FilterDriverProxy() noexcept;

    DECLARE_MESSAGE_MAP()

    // The parent
    CHidHideClientDlg& m_HidHideClientDlg;

    // Attributes
    HidHide::FullImageNames m_DropTargetFullImageNames;

    // Controls
    CListBox m_Profiles;
    CButton  m_ProfileInsert;
    CButton  m_ProfileDelete;
    CStatic  m_ApplicationsLabel;
    CListBox m_Applications;
    CButton  m_ApplicationsInsert;
    CButton  m_ApplicationsDelete;
    CStatic  m_DevicesLabel;
    CListBox m_Devices;
    CButton  m_DevicesInsert;
    CButton  m_DevicesDelete;
    CStatic  m_Guidance;

    // Events
    afx_msg void OnBnClickedButtonProfileInsert();
    afx_msg void OnBnClickedButtonProfileDelete();
    afx_msg void OnLbnSelchangeListProfiles();
    afx_msg void OnBnClickedButtonApplicationsInsert();
    afx_msg void OnBnClickedButtonApplicationsDelete();
    afx_msg void OnBnClickedButtonDevicesInsert();
    afx_msg void OnBnClickedButtonDevicesDelete();
    afx_msg void OnShowWindow(_In_ BOOL bShow, _In_ UINT nStatus);
};
