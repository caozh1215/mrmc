#pragma once

/*
 *      Copyright (C) 2017-2018 Team MrMC
 *      https://github.com/MrMC
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with MrMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "guilib/GUIControl.h"
#include "guilib/FocusabilityTracker.h"
#include "threads/CriticalSection.h"

class CAnimation;
class CGUIWindow;

typedef enum FocusEngineState
{
  Idle,
  Clear,
  Update,
} FocusEngineState;

typedef struct FocusEngineCoreItem
{
  bool IsEqual(FocusEngineCoreItem &item)
  {
    if (!IsEqualRect(item))
      return false;
    if (!IsEqualControl(item))
      return false;
    return true;
  }
  bool IsEqualRect(FocusEngineCoreItem &item)
  {
    if (rect != item.rect)
      return false;
    return true;
  }
  bool IsEqualControl(FocusEngineCoreItem &item)
  {
    if (control != item.control)
      return false;
    return true;
  }
  CRect rect;
  std::string type;
  CGUIControl *control = nullptr;
} FocusEngineCoreItem;

typedef struct FocusEngineCoreViews
{
  bool IsEqual(FocusEngineCoreViews &view)
  {
    if (!IsEqualSize(view))
      return false;
    if (!IsEqualControls(view))
      return false;
    if (!IsEqualRect(view))
      return false;
    return true;
  }
  bool IsEqualSize(FocusEngineCoreViews &view)
  {
    if (items.size() != view.items.size())
      return false;
    return true;
  }
  bool IsEqualRect(FocusEngineCoreViews &view)
  {
    if (rect != view.rect)
      return false;
    for (size_t indx = 0; indx < items.size(); ++indx)
    {
      // core always has the focused control last in any list
      // which will change the order when focus changes.
      // so we have to search for the matching control.
      auto foundItem = std::find_if(view.items.begin(), view.items.end(),
          [&](FocusEngineCoreItem item)
          { return items[indx].control == item.control;
      });
      // grr, did not find matching control in view compare
      // so punt, while size is same, controls are different.
      if (foundItem == view.items.end())
        return false;

      if (items[indx].rect != (*foundItem).rect)
        return false;
    }
    return true;
  }
  bool IsEqualControls(FocusEngineCoreViews &view)
  {
    if (control != view.control)
      return false;
    if (items.size() != view.items.size())
      return false;

    for (size_t indx = 0; indx < items.size(); ++indx)
    {
      // core always has the focused control last in any list
      // which will change the order when focus changes.
      // so we have to search for the matching control.
      auto foundItem = std::find_if(view.items.begin(), view.items.end(),
          [&](FocusEngineCoreItem item)
          { return items[indx].control == item.control;
      });
      // grr, did not find matching control in view compare
      // so punt, while size is same, controls are different.
      if (foundItem == view.items.end())
        return false;
    }
    return true;
  }
  CRect rect;
  std::string type;
  CGUIControl *control = nullptr;
  std::vector<FocusEngineCoreItem> items;
} FocusEngineCoreViews;

typedef struct FocusEngineFocus
{
  int windowID = 0;
  bool isAnimating = false;
  CGUIWindow  *window = nullptr;
  CGUIControl *rootFocus = nullptr;
  CGUIControl *itemFocus = nullptr;
  std::vector<GUIFocusabilityItem> items;
  std::vector<FocusEngineCoreViews> views;
} FocusEngineFocus;

typedef struct
{
  float zoomX = 0.0f;
  float zoomY = 0.0f;
  float slideX = 0.0f;
  float slideY = 0.0f;
  // amx amount (screen pixels) that control slides
  float maxScreenSlideX = 15.0f;
  float maxScreenSlideY = 15.0f;
} FocusEngineAnimate;

class CFocusEngineHandler
{
 public:
  static CFocusEngineHandler& GetInstance();

  void          Process();
  void          ClearAnimation();
  void          UpdateAnimation(FocusEngineAnimate &focusAnimate);
  void          EnableFocusZoom(bool enable);
  void          EnableFocusSlide(bool enable);
  void          InvalidateFocus(CGUIControl *control);
  const int     GetFocusWindowID();
  const bool    GetFocusWindowIsAnimating();
  const CRect   GetFocusRect();
  CGUIControl*  GetFocusControl();
  bool          ShowFocusRect();
  bool          ShowVisibleRects();
  ORIENTATION   GetFocusOrientation();
  void          GetCoreViews(std::vector<FocusEngineCoreViews> &views);
  static bool   CoreViewsIsEqual(std::vector<FocusEngineCoreViews> &views1, std::vector<FocusEngineCoreViews> &views2);
  static bool   CoreViewsIsEqualSize(std::vector<FocusEngineCoreViews> &views1, std::vector<FocusEngineCoreViews> &views2);
  static bool   CoreViewsIsEqualControls(std::vector<FocusEngineCoreViews> &views1, std::vector<FocusEngineCoreViews> &views2);
  void          GetGUIFocusabilityItems(std::vector<GUIFocusabilityItem> &items);
  void          SetGUIFocusabilityItems(const CFocusabilityTracker &focusabilityTracker);
  std::string   TranslateControlType(CGUIControl *control, CGUIControl *parent);

private:
  void          UpdateFocus(FocusEngineFocus &focus);
  void          UpdateFocusability();

  CFocusEngineHandler();
  CFocusEngineHandler(CFocusEngineHandler const&);
  CFocusEngineHandler& operator=(CFocusEngineHandler const&);

  bool m_focusZoom;
  bool m_focusSlide;
  bool m_showFocusRect;
  bool m_showVisibleRects;
  FocusEngineState m_state;
  CCriticalSection m_stateLock;
  FocusEngineFocus m_focus;
  CCriticalSection m_focusLock;
  ORIENTATION m_focusedOrientation;
  FocusEngineAnimate m_focusAnimate;
  static CFocusEngineHandler* m_instance;
};
