/*
 * Window position related functions.
 *
 * Copyright 1993, 1994, 1995 Alexandre Julliard
 *                       1995, 1996, 1999 Alex Korobka
 */

#include <string.h>
#include "winerror.h"
#include "windef.h"
#include "wingdi.h"
#include "winerror.h"
#include "wine/winuser16.h"
#include "controls.h"
#include "heap.h"
#include "user.h"
#include "region.h"
#include "win.h"
#include "hook.h"
#include "message.h"
#include "queue.h"
#include "options.h"
#include "winpos.h"
#include "dce.h"
#include "nonclient.h"
#include "debugtools.h"
#include "input.h"

DEFAULT_DEBUG_CHANNEL(win);

#define HAS_DLGFRAME(style,exStyle) \
    (((exStyle) & WS_EX_DLGMODALFRAME) || \
     (((style) & WS_DLGFRAME) && !((style) & WS_BORDER)))

#define HAS_THICKFRAME(style) \
    (((style) & WS_THICKFRAME) && \
     !(((style) & (WS_DLGFRAME|WS_BORDER)) == WS_DLGFRAME))

#define EMPTYPOINT(pt)          ((*(LONG*)&(pt)) == -1)

#define PLACE_MIN		0x0001
#define PLACE_MAX		0x0002
#define PLACE_RECT		0x0004

#define MINMAX_NOSWP		0x00010000


#define DWP_MAGIC  ((INT)('W' | ('P' << 8) | ('O' << 16) | ('S' << 24)))

typedef struct
{
    INT       actualCount;
    INT       suggestedCount;
    BOOL      valid;
    INT       wMagic;
    HWND      hwndParent;
    WINDOWPOS winPos[1];
} DWP;

/* ----- internal variables ----- */

static HWND hwndPrevActive  = 0;  /* Previously active window */
static HWND hGlobalShellWindow=0; /*the shell*/
static HWND hGlobalTaskmanWindow=0;
static HWND hGlobalProgmanWindow=0;

static LPCSTR atomInternalPos;

extern HQUEUE16 hActiveQueue;

/***********************************************************************
 *           WINPOS_CreateInternalPosAtom
 */
BOOL WINPOS_CreateInternalPosAtom()
{
    LPSTR str = "SysIP";
    atomInternalPos = (LPCSTR)(DWORD)GlobalAddAtomA(str);
    return (atomInternalPos) ? TRUE : FALSE;
}

/***********************************************************************
 *           WINPOS_CheckInternalPos
 *
 * Called when a window is destroyed.
 */
void WINPOS_CheckInternalPos( WND* wndPtr )
{
    LPINTERNALPOS lpPos;
    MESSAGEQUEUE *pMsgQ = 0;
    HWND hwnd = wndPtr->hwndSelf;

    lpPos = (LPINTERNALPOS) GetPropA( hwnd, atomInternalPos );

    /* Retrieve the message queue associated with this window */
    pMsgQ = (MESSAGEQUEUE *)QUEUE_Lock( wndPtr->hmemTaskQ );
    if ( !pMsgQ )
    {
        WARN("\tMessage queue not found. Exiting!\n" );
        return;
    }

    if( hwnd == hwndPrevActive ) hwndPrevActive = 0;

    if( hwnd == PERQDATA_GetActiveWnd( pMsgQ->pQData ) )
    {
        PERQDATA_SetActiveWnd( pMsgQ->pQData, 0 );
	WARN("\tattempt to activate destroyed window!\n");
    }

    if( lpPos )
    {
	if( IsWindow(lpPos->hwndIconTitle) ) 
	    DestroyWindow( lpPos->hwndIconTitle );
	HeapFree( GetProcessHeap(), 0, lpPos );
    }

    QUEUE_Unlock( pMsgQ );
    return;
}

/***********************************************************************
 *           WINPOS_FindIconPos
 *
 * Find a suitable place for an iconic window.
 */
static POINT16 WINPOS_FindIconPos( WND* wndPtr, POINT16 pt )
{
    RECT16 rectParent;
    short x, y, xspacing, yspacing;

    GetClientRect16( wndPtr->parent->hwndSelf, &rectParent );
    if ((pt.x >= rectParent.left) && (pt.x + GetSystemMetrics(SM_CXICON) < rectParent.right) &&
        (pt.y >= rectParent.top) && (pt.y + GetSystemMetrics(SM_CYICON) < rectParent.bottom))
        return pt;  /* The icon already has a suitable position */

    xspacing = GetSystemMetrics(SM_CXICONSPACING);
    yspacing = GetSystemMetrics(SM_CYICONSPACING);

    y = rectParent.bottom;
    for (;;)
    {
        x = rectParent.left;
        do 
        {
              /* Check if another icon already occupies this spot */
            WND *childPtr = WIN_LockWndPtr(wndPtr->parent->child);
            while (childPtr)
            {
                if ((childPtr->dwStyle & WS_MINIMIZE) && (childPtr != wndPtr))
                {
                    if ((childPtr->rectWindow.left < x + xspacing) &&
                        (childPtr->rectWindow.right >= x) &&
                        (childPtr->rectWindow.top <= y) &&
                        (childPtr->rectWindow.bottom > y - yspacing))
                        break;  /* There's a window in there */
                }
                WIN_UpdateWndPtr(&childPtr,childPtr->next);
            }
            WIN_ReleaseWndPtr(childPtr);
            if (!childPtr) /* No window was found, so it's OK for us */
            {
		pt.x = x + (xspacing - GetSystemMetrics(SM_CXICON)) / 2;
		pt.y = y - (yspacing + GetSystemMetrics(SM_CYICON)) / 2;
		return pt;
            }
	    x += xspacing;
        } while(x <= rectParent.right-xspacing);
        y -= yspacing;
    }
}


/***********************************************************************
 *		ArrangeIconicWindows (USER.170)
 */
UINT16 WINAPI ArrangeIconicWindows16( HWND16 parent) 
{
    return ArrangeIconicWindows(parent);
}
/***********************************************************************
 *		ArrangeIconicWindows (USER32.@)
 */
UINT WINAPI ArrangeIconicWindows( HWND parent )
{
    RECT rectParent;
    HWND hwndChild;
    INT x, y, xspacing, yspacing;

    GetClientRect( parent, &rectParent );
    x = rectParent.left;
    y = rectParent.bottom;
    xspacing = GetSystemMetrics(SM_CXICONSPACING);
    yspacing = GetSystemMetrics(SM_CYICONSPACING);

    hwndChild = GetWindow( parent, GW_CHILD );
    while (hwndChild)
    {
        if( IsIconic( hwndChild ) )
        {
            WND *wndPtr = WIN_FindWndPtr(hwndChild);
            
            WINPOS_ShowIconTitle( wndPtr, FALSE );
               
            SetWindowPos( hwndChild, 0, x + (xspacing - GetSystemMetrics(SM_CXICON)) / 2,
                            y - yspacing - GetSystemMetrics(SM_CYICON)/2, 0, 0,
                            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
	    if( IsWindow(hwndChild) )
                WINPOS_ShowIconTitle(wndPtr , TRUE );
            WIN_ReleaseWndPtr(wndPtr);

            if (x <= rectParent.right - xspacing) x += xspacing;
            else
            {
                x = rectParent.left;
                y -= yspacing;
            }
        }
        hwndChild = GetWindow( hwndChild, GW_HWNDNEXT );
    }
    return yspacing;
}


/***********************************************************************
 *		SwitchToThisWindow (USER.172)
 */
void WINAPI SwitchToThisWindow16( HWND16 hwnd, BOOL16 restore )
{
    SwitchToThisWindow( hwnd, restore );
}


/***********************************************************************
 *		SwitchToThisWindow (USER32.@)
 */
void WINAPI SwitchToThisWindow( HWND hwnd, BOOL restore )
{
    ShowWindow( hwnd, restore ? SW_RESTORE : SW_SHOWMINIMIZED );
}


/***********************************************************************
 *		GetWindowRect (USER.32)
 */
void WINAPI GetWindowRect16( HWND16 hwnd, LPRECT16 rect ) 
{
    WND * wndPtr = WIN_FindWndPtr( hwnd ); 
    if (!wndPtr) return;
    
    CONV_RECT32TO16( &wndPtr->rectWindow, rect );
    if (wndPtr->parent)
	MapWindowPoints16( wndPtr->parent->hwndSelf, 0, (POINT16 *)rect, 2 );
    WIN_ReleaseWndPtr(wndPtr);
}


/***********************************************************************
 *		GetWindowRect (USER32.@)
 */
BOOL WINAPI GetWindowRect( HWND hwnd, LPRECT rect ) 
{
    WND * wndPtr = WIN_FindWndPtr( hwnd ); 
    if (!wndPtr) return FALSE;
    
    *rect = wndPtr->rectWindow;
    if (wndPtr->parent)
	MapWindowPoints( wndPtr->parent->hwndSelf, 0, (POINT *)rect, 2 );
    WIN_ReleaseWndPtr(wndPtr);
    return TRUE;
}


/***********************************************************************
 *		GetWindowRgn (USER32.@)
 */
int WINAPI GetWindowRgn ( HWND hwnd, HRGN hrgn )
{
    int nRet = ERROR;
    WND *wndPtr = WIN_FindWndPtr( hwnd );
    if (wndPtr)
    {
        if (wndPtr->hrgnWnd) nRet = CombineRgn( hrgn, wndPtr->hrgnWnd, 0, RGN_COPY );
        WIN_ReleaseWndPtr(wndPtr);
    }
    return nRet;
}

/***********************************************************************
 *		SetWindowRgn (USER32.@)
 */
int WINAPI SetWindowRgn( HWND hwnd, HRGN hrgn, BOOL bRedraw )
{
    RECT rect;
    WND *wndPtr;
    int ret = FALSE;

    if (USER_Driver.pSetWindowRgn)
        return USER_Driver.pSetWindowRgn( hwnd, hrgn, bRedraw );

    if (!(wndPtr = WIN_FindWndPtr(hwnd))) return FALSE;

    if (wndPtr->hrgnWnd == hrgn)
    {
        ret = TRUE;
        goto done;
    }

    if (hrgn) /* verify that region really exists */
    {
        if (GetRgnBox( hrgn, &rect ) == ERROR) goto done;
    }

    if (wndPtr->hrgnWnd)
    {
        /* delete previous region */
        DeleteObject(wndPtr->hrgnWnd);
        wndPtr->hrgnWnd = 0;
    }
    wndPtr->hrgnWnd = hrgn;

    /* Size the window to the rectangle of the new region (if it isn't NULL) */
    if (hrgn) SetWindowPos( hwnd, 0, rect.left, rect.top,
                            rect.right  - rect.left, rect.bottom - rect.top,
                            SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOACTIVATE |
                            SWP_NOZORDER | (bRedraw ? 0 : SWP_NOREDRAW) );
    ret = TRUE;

 done:
    WIN_ReleaseWndPtr(wndPtr);
    return ret;
}

/***********************************************************************
 *		SetWindowRgn (USER.668)
 */
INT16 WINAPI SetWindowRgn16( HWND16 hwnd, HRGN16 hrgn,BOOL16 bRedraw)

{

  FIXME("SetWindowRgn16: stub\n"); 
  return TRUE;
}


/***********************************************************************
 *		GetClientRect (USER.33)
 */
void WINAPI GetClientRect16( HWND16 hwnd, LPRECT16 rect ) 
{
    WND * wndPtr = WIN_FindWndPtr( hwnd );

    rect->left = rect->top = rect->right = rect->bottom = 0;
    if (wndPtr) 
    {
	rect->right  = wndPtr->rectClient.right - wndPtr->rectClient.left;
	rect->bottom = wndPtr->rectClient.bottom - wndPtr->rectClient.top;
    }
    WIN_ReleaseWndPtr(wndPtr);
}


/***********************************************************************
 *		GetClientRect (USER32.@)
 */
BOOL WINAPI GetClientRect( HWND hwnd, LPRECT rect ) 
{
    WND * wndPtr = WIN_FindWndPtr( hwnd );

    rect->left = rect->top = rect->right = rect->bottom = 0;
    if (!wndPtr) return FALSE;
    rect->right  = wndPtr->rectClient.right - wndPtr->rectClient.left;
    rect->bottom = wndPtr->rectClient.bottom - wndPtr->rectClient.top;

    WIN_ReleaseWndPtr(wndPtr);
    return TRUE;
}


/*******************************************************************
 *		ClientToScreen (USER.28)
 */
void WINAPI ClientToScreen16( HWND16 hwnd, LPPOINT16 lppnt )
{
    MapWindowPoints16( hwnd, 0, lppnt, 1 );
}


/*******************************************************************
 *		ClientToScreen (USER32.@)
 */
BOOL WINAPI ClientToScreen( HWND hwnd, LPPOINT lppnt )
{
    MapWindowPoints( hwnd, 0, lppnt, 1 );
    return TRUE;
}


/*******************************************************************
 *		ScreenToClient (USER.29)
 */
void WINAPI ScreenToClient16( HWND16 hwnd, LPPOINT16 lppnt )
{
    MapWindowPoints16( 0, hwnd, lppnt, 1 );
}


/*******************************************************************
 *		ScreenToClient (USER32.@)
 */
BOOL WINAPI ScreenToClient( HWND hwnd, LPPOINT lppnt )
{
    MapWindowPoints( 0, hwnd, lppnt, 1 );
    return TRUE;
}


/***********************************************************************
 *           WINPOS_WindowFromPoint
 *
 * Find the window and hittest for a given point.
 */
INT16 WINPOS_WindowFromPoint( WND* wndScope, POINT16 pt, WND **ppWnd )
{
    WND *wndPtr;
    INT16 hittest = HTERROR;
    INT16 retvalue;
    POINT16 xy = pt;

    TRACE("scope %04x %d,%d\n", wndScope->hwndSelf, pt.x, pt.y);
   *ppWnd = NULL;
    wndPtr = WIN_LockWndPtr(wndScope->child);

    if( wndScope->dwStyle & WS_DISABLED )
    {
	    retvalue = HTERROR;
	    goto end;
    }
   
    if( wndScope->dwExStyle & WS_EX_MANAGED)
    {
	/* In managed mode we have to check wndScope first as it is also
	 * a window which received the mouse event. */

	if( pt.x < wndScope->rectClient.left || pt.x >= wndScope->rectClient.right ||
	    pt.y < wndScope->rectClient.top || pt.y >= wndScope->rectClient.bottom )
	    goto hittest;
    }
    MapWindowPoints16( GetDesktopWindow16(), wndScope->hwndSelf, &xy, 1 );

    for (;;)
    {
        while (wndPtr)
        {
            /* If point is in window, and window is visible, and it  */
            /* is enabled (or it's a top-level window), then explore */
            /* its children. Otherwise, go to the next window.       */

	     if ((wndPtr->dwStyle & WS_VISIBLE) &&
	        ((wndPtr->dwExStyle & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) != (WS_EX_LAYERED | WS_EX_TRANSPARENT)) &&
                (!(wndPtr->dwStyle & WS_DISABLED) ||
                 ((wndPtr->dwStyle & (WS_POPUP | WS_CHILD)) != WS_CHILD)) &&
		(wndPtr->hrgnWnd ?
		PtInRegion(wndPtr->hrgnWnd, xy.x - wndPtr->rectWindow.left,
			   xy.y - wndPtr->rectWindow.top) :
                ((xy.x >= wndPtr->rectWindow.left) &&
		 (xy.x < wndPtr->rectWindow.right) &&
		 (xy.y >= wndPtr->rectWindow.top) &&
		 (xy.y < wndPtr->rectWindow.bottom))))
            {
	        TRACE("%d,%d is inside %04x\n", xy.x, xy.y, wndPtr->hwndSelf);
                *ppWnd = wndPtr;  /* Got a suitable window */

                /* If window is minimized or disabled, return at once */
                if (wndPtr->dwStyle & WS_MINIMIZE)
                {
                    retvalue = HTCAPTION;
                    goto end;
                }
                if (wndPtr->dwStyle & WS_DISABLED)
                {
                    retvalue = HTERROR;
                    goto end;
                }

                /* If point is not in client area, ignore the children */
                if ((xy.x < wndPtr->rectClient.left) ||
                    (xy.x >= wndPtr->rectClient.right) ||
                    (xy.y < wndPtr->rectClient.top) ||
                    (xy.y >= wndPtr->rectClient.bottom)) break;

                xy.x -= wndPtr->rectClient.left;
                xy.y -= wndPtr->rectClient.top;
                WIN_UpdateWndPtr(&wndPtr,wndPtr->child);
            }
            else
            {
                WIN_UpdateWndPtr(&wndPtr,wndPtr->next);
            }
        }

hittest:
        /* If nothing found, try the scope window */
        if (!*ppWnd) *ppWnd = wndScope;

        /* Send the WM_NCHITTEST message (only if to the same task) */
        if ((*ppWnd)->hmemTaskQ == GetFastQueue16())
	{
            hittest = (INT16)SendMessage16( (*ppWnd)->hwndSelf, WM_NCHITTEST, 
						 0, MAKELONG( pt.x, pt.y ) );
            if (hittest != HTTRANSPARENT)
            {
                retvalue = hittest;  /* Found the window */
                goto end;
	    }
	}
        else
        {
            retvalue = HTCLIENT;
            goto end;
	}

        /* If no children found in last search, make point relative to parent */
        if (!wndPtr)
        {
            xy.x += (*ppWnd)->rectClient.left;
            xy.y += (*ppWnd)->rectClient.top;
        }

        /* Restart the search from the next sibling */
        WIN_UpdateWndPtr(&wndPtr,(*ppWnd)->next);
        *ppWnd = (*ppWnd)->parent;
    }

end:
    WIN_ReleaseWndPtr(wndPtr);
    return retvalue;
}


/*******************************************************************
 *		WindowFromPoint (USER.30)
 */
HWND16 WINAPI WindowFromPoint16( POINT16 pt )
{
    WND *pWnd;
    WINPOS_WindowFromPoint( WIN_GetDesktop(), pt, &pWnd );
    WIN_ReleaseDesktop();
    return pWnd->hwndSelf;
}


/*******************************************************************
 *		WindowFromPoint (USER32.@)
 */
HWND WINAPI WindowFromPoint( POINT pt )
{
    WND *pWnd;
    POINT16 pt16;
    CONV_POINT32TO16( &pt, &pt16 );
    WINPOS_WindowFromPoint( WIN_GetDesktop(), pt16, &pWnd );
    WIN_ReleaseDesktop();
    return (HWND)pWnd->hwndSelf;
}


/*******************************************************************
 *		ChildWindowFromPoint (USER.191)
 */
HWND16 WINAPI ChildWindowFromPoint16( HWND16 hwndParent, POINT16 pt )
{
    POINT pt32;
    CONV_POINT16TO32( &pt, &pt32 );
    return (HWND16)ChildWindowFromPoint( hwndParent, pt32 );
}


/*******************************************************************
 *		ChildWindowFromPoint (USER32.@)
 */
HWND WINAPI ChildWindowFromPoint( HWND hwndParent, POINT pt )
{
    /* pt is in the client coordinates */

    WND* wnd = WIN_FindWndPtr(hwndParent);
    RECT rect;
    HWND retvalue;

    if( !wnd ) return 0;

    /* get client rect fast */
    rect.top = rect.left = 0;
    rect.right = wnd->rectClient.right - wnd->rectClient.left;
    rect.bottom = wnd->rectClient.bottom - wnd->rectClient.top;

    if (!PtInRect( &rect, pt ))
    {
        retvalue = 0;
        goto end;
    }
    WIN_UpdateWndPtr(&wnd,wnd->child);
    while ( wnd )
    {
        if (PtInRect( &wnd->rectWindow, pt ))
        {
            retvalue = wnd->hwndSelf;
            goto end;
        }
        WIN_UpdateWndPtr(&wnd,wnd->next);
    }
    retvalue = hwndParent;
end:
    WIN_ReleaseWndPtr(wnd);
    return retvalue;
}

/*******************************************************************
 *		ChildWindowFromPointEx (USER.399)
 */
HWND16 WINAPI ChildWindowFromPointEx16( HWND16 hwndParent, POINT16 pt, UINT16 uFlags)
{
    POINT pt32;
    CONV_POINT16TO32( &pt, &pt32 );
    return (HWND16)ChildWindowFromPointEx( hwndParent, pt32, uFlags );
}


/*******************************************************************
 *		ChildWindowFromPointEx (USER32.@)
 */
HWND WINAPI ChildWindowFromPointEx( HWND hwndParent, POINT pt,
		UINT uFlags)
{
    /* pt is in the client coordinates */

    WND* wnd = WIN_FindWndPtr(hwndParent);
    RECT rect;
    HWND retvalue;

    if( !wnd ) return 0;

    /* get client rect fast */
    rect.top = rect.left = 0;
    rect.right = wnd->rectClient.right - wnd->rectClient.left;
    rect.bottom = wnd->rectClient.bottom - wnd->rectClient.top;

    if (!PtInRect( &rect, pt ))
    {
        retvalue = 0;
        goto end;
    }
    WIN_UpdateWndPtr(&wnd,wnd->child);

    while ( wnd )
    {
        if (PtInRect( &wnd->rectWindow, pt )) {
		if ( (uFlags & CWP_SKIPINVISIBLE) && 
				!(wnd->dwStyle & WS_VISIBLE) );
		else if ( (uFlags & CWP_SKIPDISABLED) && 
				(wnd->dwStyle & WS_DISABLED) );
		else if ( (uFlags & CWP_SKIPTRANSPARENT) && 
				(wnd->dwExStyle & WS_EX_TRANSPARENT) );
		else
                {
                    retvalue = wnd->hwndSelf;
                    goto end;
	        }
                
	}
	WIN_UpdateWndPtr(&wnd,wnd->next);
    }
    retvalue = hwndParent;
end:
    WIN_ReleaseWndPtr(wnd);
    return retvalue;
}


/*******************************************************************
 *         WINPOS_GetWinOffset
 *
 * Calculate the offset between the origin of the two windows. Used
 * to implement MapWindowPoints.
 */
static void WINPOS_GetWinOffset( HWND hwndFrom, HWND hwndTo,
                                 POINT *offset )
{
    WND * wndPtr = 0;

    offset->x = offset->y = 0;
    if (hwndFrom == hwndTo ) return;

      /* Translate source window origin to screen coords */
    if (hwndFrom)
    {
        if (!(wndPtr = WIN_FindWndPtr( hwndFrom )))
        {
            ERR("bad hwndFrom = %04x\n",hwndFrom);
            return;
        }
        while (wndPtr->parent)
        {
            offset->x += wndPtr->rectClient.left;
            offset->y += wndPtr->rectClient.top;
            WIN_UpdateWndPtr(&wndPtr,wndPtr->parent);
        }
        WIN_ReleaseWndPtr(wndPtr);
    }

      /* Translate origin to destination window coords */
    if (hwndTo)
    {
        if (!(wndPtr = WIN_FindWndPtr( hwndTo )))
        {
            ERR("bad hwndTo = %04x\n", hwndTo );
            return;
        }
        while (wndPtr->parent)
        {
            offset->x -= wndPtr->rectClient.left;
            offset->y -= wndPtr->rectClient.top;
            WIN_UpdateWndPtr(&wndPtr,wndPtr->parent);
        }    
        WIN_ReleaseWndPtr(wndPtr);
    }
}


/*******************************************************************
 *		MapWindowPoints (USER.258)
 */
void WINAPI MapWindowPoints16( HWND16 hwndFrom, HWND16 hwndTo,
                               LPPOINT16 lppt, UINT16 count )
{
    POINT offset;

    WINPOS_GetWinOffset( hwndFrom, hwndTo, &offset );
    while (count--)
    {
	lppt->x += offset.x;
	lppt->y += offset.y;
        lppt++;
    }
}


/*******************************************************************
 *		MapWindowPoints (USER32.@)
 */
INT WINAPI MapWindowPoints( HWND hwndFrom, HWND hwndTo,
                               LPPOINT lppt, UINT count )
{
    POINT offset;

    WINPOS_GetWinOffset( hwndFrom, hwndTo, &offset );
    while (count--)
    {
	lppt->x += offset.x;
	lppt->y += offset.y;
        lppt++;
    }
    return MAKELONG( LOWORD(offset.x), LOWORD(offset.y) );
}


/***********************************************************************
 *		IsIconic (USER.31)
 */
BOOL16 WINAPI IsIconic16(HWND16 hWnd)
{
    return IsIconic(hWnd);
}


/***********************************************************************
 *		IsIconic (USER32.@)
 */
BOOL WINAPI IsIconic(HWND hWnd)
{
    BOOL retvalue;
    WND * wndPtr = WIN_FindWndPtr(hWnd);
    if (wndPtr == NULL) return FALSE;
    retvalue = (wndPtr->dwStyle & WS_MINIMIZE) != 0;
    WIN_ReleaseWndPtr(wndPtr);
    return retvalue;
}
 
 
/***********************************************************************
 *		IsZoomed (USER.272)
 */
BOOL16 WINAPI IsZoomed16(HWND16 hWnd)
{
    return IsZoomed(hWnd);
}


/***********************************************************************
 *		IsZoomed (USER32.@)
 */
BOOL WINAPI IsZoomed(HWND hWnd)
{
    BOOL retvalue;
    WND * wndPtr = WIN_FindWndPtr(hWnd);
    if (wndPtr == NULL) return FALSE;
    retvalue = (wndPtr->dwStyle & WS_MAXIMIZE) != 0;
    WIN_ReleaseWndPtr(wndPtr);
    return retvalue;
}


/*******************************************************************
 *		GetActiveWindow (USER.60)
 */
HWND16 WINAPI GetActiveWindow16(void)
{
    return (HWND16)GetActiveWindow();
}

/*******************************************************************
 *		GetActiveWindow (USER32.@)
 */
HWND WINAPI GetActiveWindow(void)
{
    MESSAGEQUEUE *pCurMsgQ = 0;
    HWND hwndActive = 0;

    /* Get the messageQ for the current thread */
    if (!(pCurMsgQ = (MESSAGEQUEUE *)QUEUE_Lock( GetFastQueue16() )))
{
        WARN("\tCurrent message queue not found. Exiting!\n" );
        return 0;
    }

    /* Return the current active window from the perQ data of the current message Q */
    hwndActive = PERQDATA_GetActiveWnd( pCurMsgQ->pQData );

    QUEUE_Unlock( pCurMsgQ );
    return hwndActive;
}


/*******************************************************************
 *         WINPOS_CanActivate
 */
static BOOL WINPOS_CanActivate(WND* pWnd)
{
    if( pWnd && ( (pWnd->dwStyle & (WS_DISABLED | WS_VISIBLE | WS_CHILD))
       == WS_VISIBLE ) ) return TRUE;
    return FALSE;
}


/*******************************************************************
 *		SetActiveWindow (USER.59)
 */
HWND16 WINAPI SetActiveWindow16( HWND16 hwnd )
{
    return SetActiveWindow(hwnd);
}


/*******************************************************************
 *		SetActiveWindow (USER32.@)
 */
HWND WINAPI SetActiveWindow( HWND hwnd )
{
    HWND prev = 0;
    WND *wndPtr = WIN_FindWndPtr( hwnd );
    MESSAGEQUEUE *pMsgQ = 0, *pCurMsgQ = 0;

    if (!wndPtr || (wndPtr->dwStyle & (WS_DISABLED | WS_CHILD)))
    {
        prev = 0;
        goto end;
    }

    /* Get the messageQ for the current thread */
    if (!(pCurMsgQ = (MESSAGEQUEUE *)QUEUE_Lock( GetFastQueue16() )))
    {
        WARN("\tCurrent message queue not found. Exiting!\n" );
        goto CLEANUP;
    }
    
    /* Retrieve the message queue associated with this window */
    pMsgQ = (MESSAGEQUEUE *)QUEUE_Lock( wndPtr->hmemTaskQ );
    if ( !pMsgQ )
    {
        WARN("\tWindow message queue not found. Exiting!\n" );
        goto CLEANUP;
    }

    /* Make sure that the window is associated with the calling threads
     * message queue. It must share the same perQ data.
     */
    
    if ( pCurMsgQ->pQData != pMsgQ->pQData )
        goto CLEANUP;
    
    /* Save current active window */
    prev = PERQDATA_GetActiveWnd( pMsgQ->pQData );
    
    WINPOS_SetActiveWindow( hwnd, 0, 0 );

CLEANUP:
    /* Unlock the queues before returning */
    if ( pMsgQ )
        QUEUE_Unlock( pMsgQ );
    if ( pCurMsgQ )
        QUEUE_Unlock( pCurMsgQ );
    
end:
    WIN_ReleaseWndPtr(wndPtr);
    return prev;
}


/*******************************************************************
 *		GetForegroundWindow (USER.608)
 */
HWND16 WINAPI GetForegroundWindow16(void)
{
    return (HWND16)GetForegroundWindow();
}


/*******************************************************************
 *		SetForegroundWindow (USER.609)
 */
BOOL16 WINAPI SetForegroundWindow16( HWND16 hwnd )
{
    return SetForegroundWindow( hwnd );
}


/*******************************************************************
 *		GetForegroundWindow (USER32.@)
 */
HWND WINAPI GetForegroundWindow(void)
{
    HWND hwndActive = 0;

    /* Get the foreground window (active window of hActiveQueue) */
    if ( hActiveQueue )
    {
        MESSAGEQUEUE *pActiveQueue = QUEUE_Lock( hActiveQueue );
        if ( pActiveQueue )
            hwndActive = PERQDATA_GetActiveWnd( pActiveQueue->pQData );

        QUEUE_Unlock( pActiveQueue );
    }

    return hwndActive;
}

/*******************************************************************
 *		SetForegroundWindow (USER32.@)
 */
BOOL WINAPI SetForegroundWindow( HWND hwnd )
{
    return WINPOS_ChangeActiveWindow( hwnd, FALSE );
}


/*******************************************************************
 *		AllowSetForegroundWindow (USER32.@)
 */
BOOL WINAPI AllowSetForegroundWindow( DWORD procid )
{
    /* FIXME: If Win98/2000 style SetForegroundWindow behavior is
     * implemented, then fix this function. */
    return TRUE;
}


/*******************************************************************
 *		LockSetForegroundWindow (USER32.@)
 */
BOOL WINAPI LockSetForegroundWindow( UINT lockcode )
{
    /* FIXME: If Win98/2000 style SetForegroundWindow behavior is
     * implemented, then fix this function. */
    return TRUE;
}


/*******************************************************************
 *		GetShellWindow (USER.600)
 */
HWND16 WINAPI GetShellWindow16(void)
{
    return GetShellWindow();
}

/*******************************************************************
 *		SetShellWindow (USER32.@)
 */
HWND WINAPI SetShellWindow(HWND hwndshell)
{   WARN("(hWnd=%08x) semi stub\n",hwndshell );

    hGlobalShellWindow = hwndshell;
    return hGlobalShellWindow;
}


/*******************************************************************
 *		GetShellWindow (USER32.@)
 */
HWND WINAPI GetShellWindow(void)
{   WARN("(hWnd=%x) semi stub\n",hGlobalShellWindow );

    return hGlobalShellWindow;
}


/***********************************************************************
 *		BringWindowToTop (USER.45)
 */
BOOL16 WINAPI BringWindowToTop16( HWND16 hwnd )
{
    return BringWindowToTop(hwnd);
}


/***********************************************************************
 *		BringWindowToTop (USER32.@)
 */
BOOL WINAPI BringWindowToTop( HWND hwnd )
{
    return SetWindowPos( hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE );
}


/***********************************************************************
 *		MoveWindow (USER.56)
 */
BOOL16 WINAPI MoveWindow16( HWND16 hwnd, INT16 x, INT16 y, INT16 cx, INT16 cy,
                            BOOL16 repaint )
{
    return MoveWindow(hwnd,x,y,cx,cy,repaint);
}


/***********************************************************************
 *		MoveWindow (USER32.@)
 */
BOOL WINAPI MoveWindow( HWND hwnd, INT x, INT y, INT cx, INT cy,
                            BOOL repaint )
{    
    int flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (!repaint) flags |= SWP_NOREDRAW;
    TRACE("%04x %d,%d %dx%d %d\n", 
	    hwnd, x, y, cx, cy, repaint );
    return SetWindowPos( hwnd, 0, x, y, cx, cy, flags );
}

/***********************************************************************
 *           WINPOS_InitInternalPos
 */
static LPINTERNALPOS WINPOS_InitInternalPos( WND* wnd, POINT pt, 
					     LPRECT restoreRect )
{
    LPINTERNALPOS lpPos = (LPINTERNALPOS) GetPropA( wnd->hwndSelf,
                                                      atomInternalPos );
    if( !lpPos )
    {
	/* this happens when the window is minimized/maximized 
	 * for the first time (rectWindow is not adjusted yet) */

	lpPos = HeapAlloc( GetProcessHeap(), 0, sizeof(INTERNALPOS) );
	if( !lpPos ) return NULL;

	SetPropA( wnd->hwndSelf, atomInternalPos, (HANDLE)lpPos );
	lpPos->hwndIconTitle = 0; /* defer until needs to be shown */
        CONV_RECT32TO16( &wnd->rectWindow, &lpPos->rectNormal );
	*(UINT*)&lpPos->ptIconPos = *(UINT*)&lpPos->ptMaxPos = 0xFFFFFFFF;
    }

    if( wnd->dwStyle & WS_MINIMIZE ) 
	CONV_POINT32TO16( &pt, &lpPos->ptIconPos );
    else if( wnd->dwStyle & WS_MAXIMIZE ) 
	CONV_POINT32TO16( &pt, &lpPos->ptMaxPos );
    else if( restoreRect ) 
	CONV_RECT32TO16( restoreRect, &lpPos->rectNormal );

    return lpPos;
}

/***********************************************************************
 *           WINPOS_RedrawIconTitle
 */
BOOL WINPOS_RedrawIconTitle( HWND hWnd )
{
    LPINTERNALPOS lpPos = (LPINTERNALPOS)GetPropA( hWnd, atomInternalPos );
    if( lpPos )
    {
	if( lpPos->hwndIconTitle )
	{
	    SendMessageA( lpPos->hwndIconTitle, WM_SHOWWINDOW, TRUE, 0);
	    InvalidateRect( lpPos->hwndIconTitle, NULL, TRUE );
	    return TRUE;
	}
    }
    return FALSE;
}

/***********************************************************************
 *           WINPOS_ShowIconTitle
 */
BOOL WINPOS_ShowIconTitle( WND* pWnd, BOOL bShow )
{
    LPINTERNALPOS lpPos = (LPINTERNALPOS)GetPropA( pWnd->hwndSelf, atomInternalPos );

    if( lpPos && !(pWnd->dwExStyle & WS_EX_MANAGED))
    {
	HWND16 hWnd = lpPos->hwndIconTitle;

	TRACE("0x%04x %i\n", pWnd->hwndSelf, (bShow != 0) );

	if( !hWnd )
	    lpPos->hwndIconTitle = hWnd = ICONTITLE_Create( pWnd );
	if( bShow )
        {
	    if( ( pWnd = WIN_FindWndPtr(hWnd) ) != NULL) 
	    {
	        if( !(pWnd->dwStyle & WS_VISIBLE) )
		{
		   SendMessageA( hWnd, WM_SHOWWINDOW, TRUE, 0 );
		   SetWindowPos( hWnd, 0, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE |
				 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW );
		}
		WIN_ReleaseWndPtr(pWnd);
	    }
	}
	else ShowWindow( hWnd, SW_HIDE );
    }
    return FALSE;
}

/*******************************************************************
 *           WINPOS_GetMinMaxInfo
 *
 * Get the minimized and maximized information for a window.
 */
void WINPOS_GetMinMaxInfo( WND *wndPtr, POINT *maxSize, POINT *maxPos,
			   POINT *minTrack, POINT *maxTrack )
{
    LPINTERNALPOS lpPos;
    MINMAXINFO MinMax;
    INT xinc, yinc;

    /* Compute default values */

    MinMax.ptMaxSize.x = GetSystemMetrics(SM_CXSCREEN);
    MinMax.ptMaxSize.y = GetSystemMetrics(SM_CYSCREEN);
    MinMax.ptMinTrackSize.x = GetSystemMetrics(SM_CXMINTRACK);
    MinMax.ptMinTrackSize.y = GetSystemMetrics(SM_CYMINTRACK);
    MinMax.ptMaxTrackSize.x = GetSystemMetrics(SM_CXSCREEN);
    MinMax.ptMaxTrackSize.y = GetSystemMetrics(SM_CYSCREEN);

    if (wndPtr->dwExStyle & WS_EX_MANAGED) xinc = yinc = 0;
    else if (HAS_DLGFRAME( wndPtr->dwStyle, wndPtr->dwExStyle ))
    {
        xinc = GetSystemMetrics(SM_CXDLGFRAME);
        yinc = GetSystemMetrics(SM_CYDLGFRAME);
    }
    else
    {
        xinc = yinc = 0;
        if (HAS_THICKFRAME(wndPtr->dwStyle))
        {
            xinc += GetSystemMetrics(SM_CXFRAME);
            yinc += GetSystemMetrics(SM_CYFRAME);
        }
        if (wndPtr->dwStyle & WS_BORDER)
        {
            xinc += GetSystemMetrics(SM_CXBORDER);
            yinc += GetSystemMetrics(SM_CYBORDER);
        }
    }
    MinMax.ptMaxSize.x += 2 * xinc;
    MinMax.ptMaxSize.y += 2 * yinc;

    lpPos = (LPINTERNALPOS)GetPropA( wndPtr->hwndSelf, atomInternalPos );
    if( lpPos && !EMPTYPOINT(lpPos->ptMaxPos) )
	CONV_POINT16TO32( &lpPos->ptMaxPos, &MinMax.ptMaxPosition );
    else
    {
        MinMax.ptMaxPosition.x = -xinc;
        MinMax.ptMaxPosition.y = -yinc;
    }

    SendMessageA( wndPtr->hwndSelf, WM_GETMINMAXINFO, 0, (LPARAM)&MinMax );

      /* Some sanity checks */

    TRACE("%ld %ld / %ld %ld / %ld %ld / %ld %ld\n",
                      MinMax.ptMaxSize.x, MinMax.ptMaxSize.y,
                      MinMax.ptMaxPosition.x, MinMax.ptMaxPosition.y,
                      MinMax.ptMaxTrackSize.x, MinMax.ptMaxTrackSize.y,
                      MinMax.ptMinTrackSize.x, MinMax.ptMinTrackSize.y);
    MinMax.ptMaxTrackSize.x = max( MinMax.ptMaxTrackSize.x,
                                   MinMax.ptMinTrackSize.x );
    MinMax.ptMaxTrackSize.y = max( MinMax.ptMaxTrackSize.y,
                                   MinMax.ptMinTrackSize.y );

    if (maxSize) *maxSize = MinMax.ptMaxSize;
    if (maxPos) *maxPos = MinMax.ptMaxPosition;
    if (minTrack) *minTrack = MinMax.ptMinTrackSize;
    if (maxTrack) *maxTrack = MinMax.ptMaxTrackSize;
}

/***********************************************************************
 *           WINPOS_MinMaximize
 *
 * Fill in lpRect and return additional flags to be used with SetWindowPos().
 * This function assumes that 'cmd' is different from the current window
 * state.
 */
UINT WINPOS_MinMaximize( WND* wndPtr, UINT16 cmd, LPRECT16 lpRect )
{
    UINT swpFlags = 0;
    POINT pt, size;
    LPINTERNALPOS lpPos;

    TRACE("0x%04x %u\n", wndPtr->hwndSelf, cmd );

    size.x = wndPtr->rectWindow.left; size.y = wndPtr->rectWindow.top;
    lpPos = WINPOS_InitInternalPos( wndPtr, size, &wndPtr->rectWindow );

    if (lpPos && !HOOK_CallHooks16(WH_CBT, HCBT_MINMAX, wndPtr->hwndSelf, cmd))
    {
	if( wndPtr->dwStyle & WS_MINIMIZE )
	{
	    if( !SendMessageA( wndPtr->hwndSelf, WM_QUERYOPEN, 0, 0L ) )
		return (SWP_NOSIZE | SWP_NOMOVE);
	    swpFlags |= SWP_NOCOPYBITS;
	}
	switch( cmd )
	{
	    case SW_MINIMIZE:
		 if( wndPtr->dwStyle & WS_MAXIMIZE)
		 {
		     wndPtr->flags |= WIN_RESTORE_MAX;
		     wndPtr->dwStyle &= ~WS_MAXIMIZE;
                 }
                 else
		     wndPtr->flags &= ~WIN_RESTORE_MAX;
		 wndPtr->dwStyle |= WS_MINIMIZE;

                 if( wndPtr->pDriver->pSetHostAttr( wndPtr, HAK_ICONICSTATE, TRUE ) )
                     swpFlags |= MINMAX_NOSWP;

		 lpPos->ptIconPos = WINPOS_FindIconPos( wndPtr, lpPos->ptIconPos );

		 SetRect16( lpRect, lpPos->ptIconPos.x, lpPos->ptIconPos.y,
				    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON) );
		 swpFlags |= SWP_NOCOPYBITS;
		 break;

	    case SW_MAXIMIZE:
                CONV_POINT16TO32( &lpPos->ptMaxPos, &pt );
                WINPOS_GetMinMaxInfo( wndPtr, &size, &pt, NULL, NULL );
                CONV_POINT32TO16( &pt, &lpPos->ptMaxPos );

		 if( wndPtr->dwStyle & WS_MINIMIZE )
		 {
                     wndPtr->pDriver->pSetHostAttr( wndPtr, HAK_ICONICSTATE, FALSE );

		     WINPOS_ShowIconTitle( wndPtr, FALSE );
		     wndPtr->dwStyle &= ~WS_MINIMIZE;
		 }
                 wndPtr->dwStyle |= WS_MAXIMIZE;

		 SetRect16( lpRect, lpPos->ptMaxPos.x, lpPos->ptMaxPos.y,
				    size.x, size.y );
		 break;

	    case SW_RESTORE:
		 if( wndPtr->dwStyle & WS_MINIMIZE )
		 {
                     wndPtr->pDriver->pSetHostAttr( wndPtr, HAK_ICONICSTATE, FALSE );

		     wndPtr->dwStyle &= ~WS_MINIMIZE;
		     WINPOS_ShowIconTitle( wndPtr, FALSE );

		     if( wndPtr->flags & WIN_RESTORE_MAX)
		     {
			 /* Restore to maximized position */
                         CONV_POINT16TO32( &lpPos->ptMaxPos, &pt );
                         WINPOS_GetMinMaxInfo( wndPtr, &size, &pt, NULL, NULL);
                         CONV_POINT32TO16( &pt, &lpPos->ptMaxPos );
			 wndPtr->dwStyle |= WS_MAXIMIZE;
			 SetRect16( lpRect, lpPos->ptMaxPos.x, lpPos->ptMaxPos.y, size.x, size.y );
			 break;
		     }
		 } 
		 else 
		     if( !(wndPtr->dwStyle & WS_MAXIMIZE) ) return (UINT16)(-1);
 		     else wndPtr->dwStyle &= ~WS_MAXIMIZE;

		 /* Restore to normal position */

		*lpRect = lpPos->rectNormal; 
		 lpRect->right -= lpRect->left; 
		 lpRect->bottom -= lpRect->top;

		 break;
	}
    } else swpFlags |= SWP_NOSIZE | SWP_NOMOVE;
    return swpFlags;
}

/***********************************************************************
 *		ShowWindowAsync (USER32.@)
 *
 * doesn't wait; returns immediately.
 * used by threads to toggle windows in other (possibly hanging) threads
 */
BOOL WINAPI ShowWindowAsync( HWND hwnd, INT cmd )
{
    /* FIXME: does ShowWindow() return immediately ? */
    return ShowWindow(hwnd, cmd);
}


/***********************************************************************
 *		ShowWindow (USER.42)
 */
BOOL16 WINAPI ShowWindow16( HWND16 hwnd, INT16 cmd ) 
{    
    return ShowWindow(hwnd,cmd);
}


/***********************************************************************
 *		ShowWindow (USER32.@)
 */
BOOL WINAPI ShowWindow( HWND hwnd, INT cmd ) 
{    
    WND* 	wndPtr = WIN_FindWndPtr( hwnd );
    BOOL 	wasVisible, showFlag;
    RECT16 	newPos = {0, 0, 0, 0};
    UINT 	swp = 0;

    if (!wndPtr) return FALSE;

    TRACE("hwnd=%04x, cmd=%d\n", hwnd, cmd);

    wasVisible = (wndPtr->dwStyle & WS_VISIBLE) != 0;

    switch(cmd)
    {
        case SW_HIDE:
            if (!wasVisible) goto END;;
	    swp |= SWP_HIDEWINDOW | SWP_NOSIZE | SWP_NOMOVE | 
		        SWP_NOACTIVATE | SWP_NOZORDER;
	    break;

	case SW_SHOWMINNOACTIVE:
            swp |= SWP_NOACTIVATE | SWP_NOZORDER;
            /* fall through */
	case SW_SHOWMINIMIZED:
            swp |= SWP_SHOWWINDOW;
            /* fall through */
	case SW_MINIMIZE:
            swp |= SWP_FRAMECHANGED;
            if( !(wndPtr->dwStyle & WS_MINIMIZE) )
		 swp |= WINPOS_MinMaximize( wndPtr, SW_MINIMIZE, &newPos );
            else swp |= SWP_NOSIZE | SWP_NOMOVE;
	    break;

	case SW_SHOWMAXIMIZED: /* same as SW_MAXIMIZE */
            swp |= SWP_SHOWWINDOW | SWP_FRAMECHANGED;
            if( !(wndPtr->dwStyle & WS_MAXIMIZE) )
		 swp |= WINPOS_MinMaximize( wndPtr, SW_MAXIMIZE, &newPos );
            else swp |= SWP_NOSIZE | SWP_NOMOVE;
            break;

	case SW_SHOWNA:
            swp |= SWP_NOACTIVATE | SWP_NOZORDER;
            /* fall through */
	case SW_SHOW:
	    swp |= SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE;

	    /*
	     * ShowWindow has a little peculiar behavior that if the
	     * window is already the topmost window, it will not
	     * activate it.
	     */
	    if (GetTopWindow((HWND)0)==hwnd && (wasVisible || GetActiveWindow() == hwnd))
	      swp |= SWP_NOACTIVATE;

	    break;

	case SW_SHOWNOACTIVATE:
            swp |= SWP_NOZORDER;
            if (GetActiveWindow()) swp |= SWP_NOACTIVATE;
            /* fall through */
	case SW_SHOWNORMAL:  /* same as SW_NORMAL: */
	case SW_SHOWDEFAULT: /* FIXME: should have its own handler */
	case SW_RESTORE:
	    swp |= SWP_SHOWWINDOW | SWP_FRAMECHANGED;

            if( wndPtr->dwStyle & (WS_MINIMIZE | WS_MAXIMIZE) )
		 swp |= WINPOS_MinMaximize( wndPtr, SW_RESTORE, &newPos );
            else swp |= SWP_NOSIZE | SWP_NOMOVE;
	    break;
    }

    showFlag = (cmd != SW_HIDE);
    if (showFlag != wasVisible)
    {
        SendMessageA( hwnd, WM_SHOWWINDOW, showFlag, 0 );
        if (!IsWindow( hwnd )) goto END;
    }

    if ((wndPtr->dwStyle & WS_CHILD) &&
        !IsWindowVisible( wndPtr->parent->hwndSelf ) &&
        (swp & (SWP_NOSIZE | SWP_NOMOVE)) == (SWP_NOSIZE | SWP_NOMOVE) )
    {
        /* Don't call SetWindowPos() on invisible child windows */
        if (cmd == SW_HIDE) wndPtr->dwStyle &= ~WS_VISIBLE;
        else wndPtr->dwStyle |= WS_VISIBLE;
    }
    else
    {
        /* We can't activate a child window */
        if ((wndPtr->dwStyle & WS_CHILD) &&
            !(wndPtr->dwExStyle & WS_EX_MDICHILD))
            swp |= SWP_NOACTIVATE | SWP_NOZORDER;
	if (!(swp & MINMAX_NOSWP))
        {
	    SetWindowPos( hwnd, HWND_TOP, newPos.left, newPos.top, 
					  newPos.right, newPos.bottom, LOWORD(swp) );
            if (cmd == SW_HIDE)
            {
               /* FIXME: This will cause the window to be activated irrespective
                * of whether it is owned by the same thread. Has to be done
                * asynchronously.
                */

               if (hwnd == GetActiveWindow())
                  WINPOS_ActivateOtherWindow(wndPtr);

               /* Revert focus to parent */
               if (hwnd == GetFocus() || IsChild(hwnd, GetFocus()))
                  SetFocus( GetParent(hwnd) );
            }
        }
        if (!IsWindow( hwnd )) goto END;
	else if( wndPtr->dwStyle & WS_MINIMIZE ) WINPOS_ShowIconTitle( wndPtr, TRUE );
    }

    if (wndPtr->flags & WIN_NEED_SIZE)
    {
        /* should happen only in CreateWindowEx() */
	int wParam = SIZE_RESTORED;

	wndPtr->flags &= ~WIN_NEED_SIZE;
	if (wndPtr->dwStyle & WS_MAXIMIZE) wParam = SIZE_MAXIMIZED;
	else if (wndPtr->dwStyle & WS_MINIMIZE) wParam = SIZE_MINIMIZED;
	SendMessageA( hwnd, WM_SIZE, wParam,
		     MAKELONG(wndPtr->rectClient.right-wndPtr->rectClient.left,
			    wndPtr->rectClient.bottom-wndPtr->rectClient.top));
	SendMessageA( hwnd, WM_MOVE, 0,
		   MAKELONG(wndPtr->rectClient.left, wndPtr->rectClient.top) );
    }

END:
    WIN_ReleaseWndPtr(wndPtr);
    return wasVisible;
}


/***********************************************************************
 *		GetInternalWindowPos (USER.460)
 */
UINT16 WINAPI GetInternalWindowPos16( HWND16 hwnd, LPRECT16 rectWnd,
                                      LPPOINT16 ptIcon )
{
    WINDOWPLACEMENT16 wndpl;
    if (GetWindowPlacement16( hwnd, &wndpl )) 
    {
	if (rectWnd) *rectWnd = wndpl.rcNormalPosition;
	if (ptIcon)  *ptIcon = wndpl.ptMinPosition;
	return wndpl.showCmd;
    }
    return 0;
}


/***********************************************************************
 *		GetInternalWindowPos (USER32.@)
 */
UINT WINAPI GetInternalWindowPos( HWND hwnd, LPRECT rectWnd,
                                      LPPOINT ptIcon )
{
    WINDOWPLACEMENT wndpl;
    if (GetWindowPlacement( hwnd, &wndpl ))
    {
	if (rectWnd) *rectWnd = wndpl.rcNormalPosition;
	if (ptIcon)  *ptIcon = wndpl.ptMinPosition;
	return wndpl.showCmd;
    }
    return 0;
}

/***********************************************************************
 *		GetWindowPlacement (USER.370)
 */
BOOL16 WINAPI GetWindowPlacement16( HWND16 hwnd, WINDOWPLACEMENT16 *wndpl )
{
    WND *pWnd = WIN_FindWndPtr( hwnd );
    LPINTERNALPOS lpPos;
    
    if(!pWnd ) return FALSE;

    lpPos = (LPINTERNALPOS)WINPOS_InitInternalPos( pWnd,
			     *(LPPOINT)&pWnd->rectWindow.left, &pWnd->rectWindow );
	wndpl->length  = sizeof(*wndpl);
	if( pWnd->dwStyle & WS_MINIMIZE )
	    wndpl->showCmd = SW_SHOWMINIMIZED;
	else 
	    wndpl->showCmd = ( pWnd->dwStyle & WS_MAXIMIZE )
			     ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL ;
	if( pWnd->flags & WIN_RESTORE_MAX )
	    wndpl->flags = WPF_RESTORETOMAXIMIZED;
	else
	    wndpl->flags = 0;
	wndpl->ptMinPosition = lpPos->ptIconPos;
	wndpl->ptMaxPosition = lpPos->ptMaxPos;
	wndpl->rcNormalPosition = lpPos->rectNormal;

    WIN_ReleaseWndPtr(pWnd);
	return TRUE;
    }


/***********************************************************************
 *		GetWindowPlacement (USER32.@)
 *
 * Win95:
 * Fails if wndpl->length of Win95 (!) apps is invalid.
 */
BOOL WINAPI GetWindowPlacement( HWND hwnd, WINDOWPLACEMENT *pwpl32 )
{
    if( pwpl32 )
    {
	WINDOWPLACEMENT16 wpl;
	wpl.length = sizeof(wpl);
        if( GetWindowPlacement16( hwnd, &wpl ) )
	{
	    pwpl32->length = sizeof(*pwpl32);
	    pwpl32->flags = wpl.flags;
	    pwpl32->showCmd = wpl.showCmd;
	    CONV_POINT16TO32( &wpl.ptMinPosition, &pwpl32->ptMinPosition );
	    CONV_POINT16TO32( &wpl.ptMaxPosition, &pwpl32->ptMaxPosition );
	    CONV_RECT16TO32( &wpl.rcNormalPosition, &pwpl32->rcNormalPosition );
	    return TRUE;
	}
    }
    return FALSE;
}


/***********************************************************************
 *           WINPOS_SetPlacement
 */
static BOOL WINPOS_SetPlacement( HWND hwnd, const WINDOWPLACEMENT16 *wndpl,
						UINT flags )
{
    WND *pWnd = WIN_FindWndPtr( hwnd );
    if( pWnd )
    {
	LPINTERNALPOS lpPos = (LPINTERNALPOS)WINPOS_InitInternalPos( pWnd,
			     *(LPPOINT)&pWnd->rectWindow.left, &pWnd->rectWindow );

	if( flags & PLACE_MIN ) lpPos->ptIconPos = wndpl->ptMinPosition;
	if( flags & PLACE_MAX ) lpPos->ptMaxPos = wndpl->ptMaxPosition;
	if( flags & PLACE_RECT) lpPos->rectNormal = wndpl->rcNormalPosition;

	if( pWnd->dwStyle & WS_MINIMIZE )
	{
	    WINPOS_ShowIconTitle( pWnd, FALSE );
	    if( wndpl->flags & WPF_SETMINPOSITION && !EMPTYPOINT(lpPos->ptIconPos))
		SetWindowPos( hwnd, 0, lpPos->ptIconPos.x, lpPos->ptIconPos.y,
				0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
	} 
	else if( pWnd->dwStyle & WS_MAXIMIZE )
	{
	    if( !EMPTYPOINT(lpPos->ptMaxPos) )
		SetWindowPos( hwnd, 0, lpPos->ptMaxPos.x, lpPos->ptMaxPos.y,
				0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
	}
	else if( flags & PLACE_RECT )
		SetWindowPos( hwnd, 0, lpPos->rectNormal.left, lpPos->rectNormal.top,
				lpPos->rectNormal.right - lpPos->rectNormal.left,
				lpPos->rectNormal.bottom - lpPos->rectNormal.top,
				SWP_NOZORDER | SWP_NOACTIVATE );

	ShowWindow( hwnd, wndpl->showCmd );
	if( IsWindow(hwnd) && pWnd->dwStyle & WS_MINIMIZE )
	{
	    if( pWnd->dwStyle & WS_VISIBLE ) WINPOS_ShowIconTitle( pWnd, TRUE );

	    /* SDK: ...valid only the next time... */
	    if( wndpl->flags & WPF_RESTORETOMAXIMIZED ) pWnd->flags |= WIN_RESTORE_MAX;
	}
        WIN_ReleaseWndPtr(pWnd);
	return TRUE;
    }
    return FALSE;
}


/***********************************************************************
 *		SetWindowPlacement (USER.371)
 */
BOOL16 WINAPI SetWindowPlacement16(HWND16 hwnd, const WINDOWPLACEMENT16 *wndpl)
{
    return WINPOS_SetPlacement( hwnd, wndpl,
                                PLACE_MIN | PLACE_MAX | PLACE_RECT );
}

/***********************************************************************
 *		SetWindowPlacement (USER32.@)
 *
 * Win95:
 * Fails if wndpl->length of Win95 (!) apps is invalid.
 */
BOOL WINAPI SetWindowPlacement( HWND hwnd, const WINDOWPLACEMENT *pwpl32 )
{
    if( pwpl32 )
    {
	WINDOWPLACEMENT16 wpl;

	wpl.length = sizeof(WINDOWPLACEMENT16);
	wpl.flags = pwpl32->flags;
	wpl.showCmd = pwpl32->showCmd;
	wpl.ptMinPosition.x = pwpl32->ptMinPosition.x;
	wpl.ptMinPosition.y = pwpl32->ptMinPosition.y;
	wpl.ptMaxPosition.x = pwpl32->ptMaxPosition.x;
	wpl.ptMaxPosition.y = pwpl32->ptMaxPosition.y;
	wpl.rcNormalPosition.left = pwpl32->rcNormalPosition.left;
	wpl.rcNormalPosition.top = pwpl32->rcNormalPosition.top;
	wpl.rcNormalPosition.right = pwpl32->rcNormalPosition.right;
	wpl.rcNormalPosition.bottom = pwpl32->rcNormalPosition.bottom;

        return WINPOS_SetPlacement( hwnd, &wpl, PLACE_MIN | PLACE_MAX | PLACE_RECT );
    }
    return FALSE;
}


/***********************************************************************
 *		SetInternalWindowPos (USER.461)
 */
void WINAPI SetInternalWindowPos16( HWND16 hwnd, UINT16 showCmd,
                                    LPRECT16 rect, LPPOINT16 pt )
{
    if( IsWindow16(hwnd) )
    {
	WINDOWPLACEMENT16 wndpl;
	UINT flags;

	wndpl.length  = sizeof(wndpl);
	wndpl.showCmd = showCmd;
	wndpl.flags = flags = 0;

	if( pt )
	{
	    flags |= PLACE_MIN;
	    wndpl.flags |= WPF_SETMINPOSITION;
	    wndpl.ptMinPosition = *pt;
	}
	if( rect )
	{
	    flags |= PLACE_RECT;
	    wndpl.rcNormalPosition = *rect;
	}
	WINPOS_SetPlacement( hwnd, &wndpl, flags );
    }
}


/***********************************************************************
 *		SetInternalWindowPos (USER32.@)
 */
void WINAPI SetInternalWindowPos( HWND hwnd, UINT showCmd,
                                    LPRECT rect, LPPOINT pt )
{
    if( IsWindow(hwnd) )
    {
	WINDOWPLACEMENT16 wndpl;
	UINT flags;

	wndpl.length  = sizeof(wndpl);
	wndpl.showCmd = showCmd;
	wndpl.flags = flags = 0;

	if( pt )
	{
            flags |= PLACE_MIN;
            wndpl.flags |= WPF_SETMINPOSITION;
            CONV_POINT32TO16( pt, &wndpl.ptMinPosition );
	}
	if( rect )
	{
            flags |= PLACE_RECT;
            CONV_RECT32TO16( rect, &wndpl.rcNormalPosition );
	}
        WINPOS_SetPlacement( hwnd, &wndpl, flags );
    }
}

/*******************************************************************
 *	   WINPOS_SetActiveWindow
 *
 * SetActiveWindow() back-end. This is the only function that
 * can assign active status to a window. It must be called only
 * for the top level windows.
 */
BOOL WINPOS_SetActiveWindow( HWND hWnd, BOOL fMouse, BOOL fChangeFocus)
{
    CBTACTIVATESTRUCT16* cbtStruct;
    WND*     wndPtr=0, *wndTemp;
    HQUEUE16 hOldActiveQueue, hNewActiveQueue;
    MESSAGEQUEUE *pOldActiveQueue = 0, *pNewActiveQueue = 0;
    WORD     wIconized = 0;
    HWND     hwndActive = 0;
    BOOL     bRet = 0;

    TRACE("(%04x, %d, %d)\n", hWnd, fMouse, fChangeFocus );

    /* Get current active window from the active queue */
    if ( hActiveQueue )
    {
        pOldActiveQueue = QUEUE_Lock( hActiveQueue );
        if ( pOldActiveQueue )
            hwndActive = PERQDATA_GetActiveWnd( pOldActiveQueue->pQData );
    }

    /* paranoid checks */
    if( hWnd == GetDesktopWindow() || (bRet = (hWnd == hwndActive)) )
	goto CLEANUP_END;

/*  if (wndPtr && (GetFastQueue16() != wndPtr->hmemTaskQ))
 *	return 0;
 */
    wndPtr = WIN_FindWndPtr(hWnd);
    hOldActiveQueue = hActiveQueue;

    if( (wndTemp = WIN_FindWndPtr(hwndActive)) )
    {
	wIconized = HIWORD(wndTemp->dwStyle & WS_MINIMIZE);
        WIN_ReleaseWndPtr(wndTemp);
    }
    else
	TRACE("no current active window.\n");

    /* call CBT hook chain */
    if ((cbtStruct = SEGPTR_NEW(CBTACTIVATESTRUCT16)))
    {
        cbtStruct->fMouse     = fMouse;
        cbtStruct->hWndActive = hwndActive;
        bRet = (BOOL)HOOK_CallHooks16( WH_CBT, HCBT_ACTIVATE, (WPARAM16)hWnd,
                                     (LPARAM)SEGPTR_GET(cbtStruct) );
        SEGPTR_FREE(cbtStruct);
        if (bRet) goto CLEANUP_END;
    }

    /* set prev active wnd to current active wnd and send notification */
    if ((hwndPrevActive = hwndActive) && IsWindow(hwndPrevActive))
    {
        MESSAGEQUEUE *pTempActiveQueue = 0;
        
        if (!SendMessageA( hwndPrevActive, WM_NCACTIVATE, FALSE, 0 ))
        {
	    if (GetSysModalWindow16() != hWnd) 
		goto CLEANUP_END;
	    /* disregard refusal if hWnd is sysmodal */
        }

	SendMessageA( hwndPrevActive, WM_ACTIVATE,
                        MAKEWPARAM( WA_INACTIVE, wIconized ),
                        (LPARAM)hWnd );

        /* check if something happened during message processing
         * (global active queue may have changed)
         */
        pTempActiveQueue = QUEUE_Lock( hActiveQueue );
	if(!pTempActiveQueue)
	    goto CLEANUP_END;

        hwndActive = PERQDATA_GetActiveWnd( pTempActiveQueue->pQData );
        QUEUE_Unlock( pTempActiveQueue );
        if( hwndPrevActive != hwndActive )
            goto CLEANUP_END;
    }

    /* Set new active window in the message queue */
    hwndActive = hWnd;
    if ( wndPtr )
    {
        pNewActiveQueue = QUEUE_Lock( wndPtr->hmemTaskQ );
        if ( pNewActiveQueue )
            PERQDATA_SetActiveWnd( pNewActiveQueue->pQData, hwndActive );
    }
    else /* have to do this or MDI frame activation goes to hell */
	if( pOldActiveQueue )
	    PERQDATA_SetActiveWnd( pOldActiveQueue->pQData, 0 );

    /* send palette messages */
    if (hWnd && SendMessage16( hWnd, WM_QUERYNEWPALETTE, 0, 0L))
	SendMessage16((HWND16)-1, WM_PALETTEISCHANGING, (WPARAM16)hWnd, 0L );

    /* if prev wnd is minimized redraw icon title */
    if( IsIconic( hwndPrevActive ) ) WINPOS_RedrawIconTitle(hwndPrevActive);

    /* managed windows will get ConfigureNotify event */  
    if (wndPtr && !(wndPtr->dwStyle & WS_CHILD) && !(wndPtr->dwExStyle & WS_EX_MANAGED))
    {
	/* check Z-order and bring hWnd to the top */
	for (wndTemp = WIN_LockWndPtr(WIN_GetDesktop()->child); wndTemp; WIN_UpdateWndPtr(&wndTemp,wndTemp->next))
        {
	    if (wndTemp->dwStyle & WS_VISIBLE) break;
        }
        WIN_ReleaseDesktop();
        WIN_ReleaseWndPtr(wndTemp);

	if( wndTemp != wndPtr )
	    SetWindowPos(hWnd, HWND_TOP, 0,0,0,0, 
			   SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE );
        if (!IsWindow(hWnd))  
	    goto CLEANUP;
    }

    /* Get a handle to the new active queue */
    hNewActiveQueue = wndPtr ? wndPtr->hmemTaskQ : 0;

    /* send WM_ACTIVATEAPP if necessary */
    if (hOldActiveQueue != hNewActiveQueue)
    {
        WND **list, **ppWnd;
        WND *pDesktop = WIN_GetDesktop();

        if ((list = WIN_BuildWinArray( pDesktop, 0, NULL )))
        {
            for (ppWnd = list; *ppWnd; ppWnd++)
            {
                if (!IsWindow( (*ppWnd)->hwndSelf )) continue;

                if ((*ppWnd)->hmemTaskQ == hOldActiveQueue)
                   SendMessage16( (*ppWnd)->hwndSelf, WM_ACTIVATEAPP,
                                   0, QUEUE_GetQueueTask(hNewActiveQueue) );
            }
            WIN_ReleaseWinArray(list);
        }

	hActiveQueue = hNewActiveQueue;

        if ((list = WIN_BuildWinArray(pDesktop, 0, NULL )))
        {
            for (ppWnd = list; *ppWnd; ppWnd++)
            {
                if (!IsWindow( (*ppWnd)->hwndSelf )) continue;

                if ((*ppWnd)->hmemTaskQ == hNewActiveQueue)
                   SendMessage16( (*ppWnd)->hwndSelf, WM_ACTIVATEAPP,
                                  1, QUEUE_GetQueueTask( hOldActiveQueue ) );
            }
            WIN_ReleaseWinArray(list);
        }
        WIN_ReleaseDesktop();
        
	if (hWnd && !IsWindow(hWnd)) goto CLEANUP;
    }

    if (hWnd)
    {
        /* walk up to the first unowned window */
        wndTemp = WIN_LockWndPtr(wndPtr);
        while (wndTemp->owner)
        {
            WIN_UpdateWndPtr(&wndTemp,wndTemp->owner);
        }
        /* and set last active owned popup */
        wndTemp->hwndLastActive = hWnd;

        wIconized = HIWORD(wndTemp->dwStyle & WS_MINIMIZE);
        WIN_ReleaseWndPtr(wndTemp);
        SendMessageA( hWnd, WM_NCACTIVATE, TRUE, 0 );
        SendMessageA( hWnd, WM_ACTIVATE,
		 MAKEWPARAM( (fMouse) ? WA_CLICKACTIVE : WA_ACTIVE, wIconized),
		 (LPARAM)hwndPrevActive );
        if( !IsWindow(hWnd) ) goto CLEANUP;
    }

    /* change focus if possible */
    if ( fChangeFocus )
    {
        if ( pNewActiveQueue )
        {
            HWND hOldFocus = PERQDATA_GetFocusWnd( pNewActiveQueue->pQData );

            if ( hOldFocus && WIN_GetTopParent( hOldFocus ) != hwndActive )
                FOCUS_SwitchFocus( pNewActiveQueue, hOldFocus, 
                                   (wndPtr && (wndPtr->dwStyle & WS_MINIMIZE))?
                                   0 : hwndActive );
        }

        if ( pOldActiveQueue && 
             ( !pNewActiveQueue || 
                pNewActiveQueue->pQData != pOldActiveQueue->pQData ) )
        {
            HWND hOldFocus = PERQDATA_GetFocusWnd( pOldActiveQueue->pQData );
            if ( hOldFocus )
                FOCUS_SwitchFocus( pOldActiveQueue, hOldFocus, 0 );
        }
    }

    if( !hwndPrevActive && wndPtr )
        (*wndPtr->pDriver->pForceWindowRaise)(wndPtr);

    /* if active wnd is minimized redraw icon title */
    if( IsIconic(hwndActive) ) WINPOS_RedrawIconTitle(hwndActive);

    bRet = (hWnd == hwndActive);  /* Success? */
    
CLEANUP: /* Unlock the message queues before returning */

    if ( pNewActiveQueue )
        QUEUE_Unlock( pNewActiveQueue );

CLEANUP_END:

    if ( pOldActiveQueue )
        QUEUE_Unlock( pOldActiveQueue );

    WIN_ReleaseWndPtr(wndPtr);
    return bRet;
}

/*******************************************************************
 *         WINPOS_ActivateOtherWindow
 *
 *  Activates window other than pWnd.
 */
BOOL WINPOS_ActivateOtherWindow(WND* pWnd)
{
  BOOL	bRet = 0;
  WND*  	pWndTo = NULL;
    HWND       hwndActive = 0;

    /* Get current active window from the active queue */
    if ( hActiveQueue )
    {
        MESSAGEQUEUE *pActiveQueue = QUEUE_Lock( hActiveQueue );
        if ( pActiveQueue )
        {
            hwndActive = PERQDATA_GetActiveWnd( pActiveQueue->pQData );
            QUEUE_Unlock( pActiveQueue );
        }
    }

  if( pWnd->hwndSelf == hwndPrevActive )
      hwndPrevActive = 0;

  if( hwndActive != pWnd->hwndSelf && 
    ( hwndActive || QUEUE_IsExitingQueue(pWnd->hmemTaskQ)) )
      return 0;

  if( !(pWnd->dwStyle & WS_POPUP) || !(pWnd->owner) ||
      !WINPOS_CanActivate((pWndTo = WIN_GetTopParentPtr(pWnd->owner))) ) 
  {
      WND* pWndPtr = WIN_GetTopParentPtr(pWnd);

      WIN_ReleaseWndPtr(pWndTo);
      pWndTo = WIN_FindWndPtr(hwndPrevActive);

      while( !WINPOS_CanActivate(pWndTo) ) 
      {
	 /* by now owned windows should've been taken care of */
          WIN_UpdateWndPtr(&pWndTo,pWndPtr->next);
          WIN_UpdateWndPtr(&pWndPtr,pWndTo);
	  if( !pWndTo ) break;
      }
      WIN_ReleaseWndPtr(pWndPtr);
  }

  bRet = WINPOS_SetActiveWindow( pWndTo ? pWndTo->hwndSelf : 0, FALSE, TRUE );

  /* switch desktop queue to current active */
  if( pWndTo )
  {
      WIN_GetDesktop()->hmemTaskQ = pWndTo->hmemTaskQ;
      WIN_ReleaseWndPtr(pWndTo);
      WIN_ReleaseDesktop();
  }

  hwndPrevActive = 0;
  return bRet;  
}

/*******************************************************************
 *	   WINPOS_ChangeActiveWindow
 *
 */
BOOL WINPOS_ChangeActiveWindow( HWND hWnd, BOOL mouseMsg )
{
    WND *wndPtr, *wndTemp;
    BOOL retvalue;
    HWND hwndActive = 0;

    /* Get current active window from the active queue */
    if ( hActiveQueue )
    {
        MESSAGEQUEUE *pActiveQueue = QUEUE_Lock( hActiveQueue );
        if ( pActiveQueue )
        {
            hwndActive = PERQDATA_GetActiveWnd( pActiveQueue->pQData );
            QUEUE_Unlock( pActiveQueue );
        }
    }

    if (!hWnd)
        return WINPOS_SetActiveWindow( 0, mouseMsg, TRUE );

    wndPtr = WIN_FindWndPtr(hWnd);
    if( !wndPtr ) return FALSE;

    /* child windows get WM_CHILDACTIVATE message */
    if( (wndPtr->dwStyle & (WS_CHILD | WS_POPUP)) == WS_CHILD )
    {
        retvalue = SendMessageA(hWnd, WM_CHILDACTIVATE, 0, 0L);
        goto end;
    }

    if( hWnd == hwndActive )
    {
        retvalue = FALSE;
        goto end;
    }

    if( !WINPOS_SetActiveWindow(hWnd ,mouseMsg ,TRUE) )
    {
        retvalue = FALSE;
        goto end;
    }

    /* switch desktop queue to current active */
    wndTemp = WIN_GetDesktop();
    if( wndPtr->parent == wndTemp)
        wndTemp->hmemTaskQ = wndPtr->hmemTaskQ;
    WIN_ReleaseDesktop();

    retvalue = TRUE;
end:
    WIN_ReleaseWndPtr(wndPtr);
    return retvalue;
}


/***********************************************************************
 *           WINPOS_SendNCCalcSize
 *
 * Send a WM_NCCALCSIZE message to a window.
 * All parameters are read-only except newClientRect.
 * oldWindowRect, oldClientRect and winpos must be non-NULL only
 * when calcValidRect is TRUE.
 */
LONG WINPOS_SendNCCalcSize( HWND hwnd, BOOL calcValidRect,
                            RECT *newWindowRect, RECT *oldWindowRect,
                            RECT *oldClientRect, WINDOWPOS *winpos,
                            RECT *newClientRect )
{
    NCCALCSIZE_PARAMS params;
    WINDOWPOS winposCopy;
    LONG result;

    params.rgrc[0] = *newWindowRect;
    if (calcValidRect)
    {
        winposCopy = *winpos;
	params.rgrc[1] = *oldWindowRect;
	params.rgrc[2] = *oldClientRect;
	params.lppos = &winposCopy;
    }
    result = SendMessageA( hwnd, WM_NCCALCSIZE, calcValidRect,
                             (LPARAM)&params );
    TRACE("%d,%d-%d,%d\n",
                 params.rgrc[0].left, params.rgrc[0].top,
                 params.rgrc[0].right, params.rgrc[0].bottom );

    /* If the application send back garbage, ignore it */
    if (params.rgrc[0].left <= params.rgrc[0].right && params.rgrc[0].top <= params.rgrc[0].bottom)
        *newClientRect = params.rgrc[0];

    return result;
}


/***********************************************************************
 *           WINPOS_HandleWindowPosChanging16
 *
 * Default handling for a WM_WINDOWPOSCHANGING. Called from DefWindowProc().
 */
LONG WINPOS_HandleWindowPosChanging16( WND *wndPtr, WINDOWPOS16 *winpos )
{
    POINT maxSize, minTrack;
    if (winpos->flags & SWP_NOSIZE) return 0;
    if ((wndPtr->dwStyle & WS_THICKFRAME) ||
	((wndPtr->dwStyle & (WS_POPUP | WS_CHILD)) == 0))
    {
	WINPOS_GetMinMaxInfo( wndPtr, &maxSize, NULL, &minTrack, NULL );
	if (maxSize.x < winpos->cx) winpos->cx = maxSize.x;
	if (maxSize.y < winpos->cy) winpos->cy = maxSize.y;
	if (!(wndPtr->dwStyle & WS_MINIMIZE))
	{
	    if (winpos->cx < minTrack.x ) winpos->cx = minTrack.x;
	    if (winpos->cy < minTrack.y ) winpos->cy = minTrack.y;
	}
    }
    return 0;
}


/***********************************************************************
 *           WINPOS_HandleWindowPosChanging
 *
 * Default handling for a WM_WINDOWPOSCHANGING. Called from DefWindowProc().
 */
LONG WINPOS_HandleWindowPosChanging( WND *wndPtr, WINDOWPOS *winpos )
{
    POINT maxSize, minTrack;
    if (winpos->flags & SWP_NOSIZE) return 0;
    if ((wndPtr->dwStyle & WS_THICKFRAME) ||
	((wndPtr->dwStyle & (WS_POPUP | WS_CHILD)) == 0))
    {
	WINPOS_GetMinMaxInfo( wndPtr, &maxSize, NULL, &minTrack, NULL );
	winpos->cx = min( winpos->cx, maxSize.x );
	winpos->cy = min( winpos->cy, maxSize.y );
	if (!(wndPtr->dwStyle & WS_MINIMIZE))
	{
	    if (winpos->cx < minTrack.x ) winpos->cx = minTrack.x;
	    if (winpos->cy < minTrack.y ) winpos->cy = minTrack.y;
	}
    }
    return 0;
}

/***********************************************************************
 *		SetWindowPos (USER.232)
 */
BOOL16 WINAPI SetWindowPos16( HWND16 hwnd, HWND16 hwndInsertAfter,
                              INT16 x, INT16 y, INT16 cx, INT16 cy, WORD flags)
{
    return SetWindowPos(hwnd,(INT)(INT16)hwndInsertAfter,x,y,cx,cy,flags);
}

/***********************************************************************
 *		SetWindowPos (USER32.@)
 */
BOOL WINAPI SetWindowPos( HWND hwnd, HWND hwndInsertAfter,
                          INT x, INT y, INT cx, INT cy, UINT flags )
{
    WINDOWPOS winpos;

    winpos.hwnd = hwnd;
    winpos.hwndInsertAfter = hwndInsertAfter;
    winpos.x = x;
    winpos.y = y;
    winpos.cx = cx;
    winpos.cy = cy;
    winpos.flags = flags;
    return USER_Driver.pSetWindowPos( &winpos );
}


/***********************************************************************
 *		BeginDeferWindowPos (USER.259)
 */
HDWP16 WINAPI BeginDeferWindowPos16( INT16 count )
{
    return BeginDeferWindowPos( count );
}


/***********************************************************************
 *		BeginDeferWindowPos (USER32.@)
 */
HDWP WINAPI BeginDeferWindowPos( INT count )
{
    HDWP handle;
    DWP *pDWP;

    if (count < 0) 
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* Windows allows zero count, in which case it allocates context for 8 moves */
    if (count == 0) count = 8;

    handle = USER_HEAP_ALLOC( sizeof(DWP) + (count-1)*sizeof(WINDOWPOS) );
    if (!handle) return 0;
    pDWP = (DWP *) USER_HEAP_LIN_ADDR( handle );
    pDWP->actualCount    = 0;
    pDWP->suggestedCount = count;
    pDWP->valid          = TRUE;
    pDWP->wMagic         = DWP_MAGIC;
    pDWP->hwndParent     = 0;
    return handle;
}


/***********************************************************************
 *		DeferWindowPos (USER.260)
 */
HDWP16 WINAPI DeferWindowPos16( HDWP16 hdwp, HWND16 hwnd, HWND16 hwndAfter,
                                INT16 x, INT16 y, INT16 cx, INT16 cy,
                                UINT16 flags )
{
    return DeferWindowPos( hdwp, hwnd, (INT)(INT16)hwndAfter,
                             x, y, cx, cy, flags );
}


/***********************************************************************
 *		DeferWindowPos (USER32.@)
 */
HDWP WINAPI DeferWindowPos( HDWP hdwp, HWND hwnd, HWND hwndAfter,
                                INT x, INT y, INT cx, INT cy,
                                UINT flags )
{
    DWP *pDWP;
    int i;
    HDWP newhdwp = hdwp,retvalue;
    /* HWND parent; */
    WND *pWnd;

    pDWP = (DWP *) USER_HEAP_LIN_ADDR( hdwp );
    if (!pDWP) return 0;
    if (hwnd == GetDesktopWindow()) return 0;

    if (!(pWnd=WIN_FindWndPtr( hwnd ))) {
        USER_HEAP_FREE( hdwp );
        return 0;
    }
    	
/* Numega Bounds Checker Demo dislikes the following code.
   In fact, I've not been able to find any "same parent" requirement in any docu
   [AM 980509]
 */
#if 0
    /* All the windows of a DeferWindowPos() must have the same parent */
    parent = pWnd->parent->hwndSelf;
    if (pDWP->actualCount == 0) pDWP->hwndParent = parent;
    else if (parent != pDWP->hwndParent)
    {
        USER_HEAP_FREE( hdwp );
        retvalue = 0;
        goto END;
    }
#endif

    for (i = 0; i < pDWP->actualCount; i++)
    {
        if (pDWP->winPos[i].hwnd == hwnd)
        {
              /* Merge with the other changes */
            if (!(flags & SWP_NOZORDER))
            {
                pDWP->winPos[i].hwndInsertAfter = hwndAfter;
            }
            if (!(flags & SWP_NOMOVE))
            {
                pDWP->winPos[i].x = x;
                pDWP->winPos[i].y = y;
            }                
            if (!(flags & SWP_NOSIZE))
            {
                pDWP->winPos[i].cx = cx;
                pDWP->winPos[i].cy = cy;
            }
            pDWP->winPos[i].flags &= flags | ~(SWP_NOSIZE | SWP_NOMOVE |
                                               SWP_NOZORDER | SWP_NOREDRAW |
                                               SWP_NOACTIVATE | SWP_NOCOPYBITS|
                                               SWP_NOOWNERZORDER);
            pDWP->winPos[i].flags |= flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW |
                                              SWP_FRAMECHANGED);
            retvalue = hdwp;
            goto END;
        }
    }
    if (pDWP->actualCount >= pDWP->suggestedCount)
    {
        newhdwp = USER_HEAP_REALLOC( hdwp,
                      sizeof(DWP) + pDWP->suggestedCount*sizeof(WINDOWPOS) );
        if (!newhdwp)
        {
            retvalue = 0;
            goto END;
        }
        pDWP = (DWP *) USER_HEAP_LIN_ADDR( newhdwp );
        pDWP->suggestedCount++;
    }
    pDWP->winPos[pDWP->actualCount].hwnd = hwnd;
    pDWP->winPos[pDWP->actualCount].hwndInsertAfter = hwndAfter;
    pDWP->winPos[pDWP->actualCount].x = x;
    pDWP->winPos[pDWP->actualCount].y = y;
    pDWP->winPos[pDWP->actualCount].cx = cx;
    pDWP->winPos[pDWP->actualCount].cy = cy;
    pDWP->winPos[pDWP->actualCount].flags = flags;
    pDWP->actualCount++;
    retvalue = newhdwp;
END:
    WIN_ReleaseWndPtr(pWnd);
    return retvalue;
}


/***********************************************************************
 *		EndDeferWindowPos (USER.261)
 */
BOOL16 WINAPI EndDeferWindowPos16( HDWP16 hdwp )
{
    return EndDeferWindowPos( hdwp );
}


/***********************************************************************
 *		EndDeferWindowPos (USER32.@)
 */
BOOL WINAPI EndDeferWindowPos( HDWP hdwp )
{
    DWP *pDWP;
    WINDOWPOS *winpos;
    BOOL res = TRUE;
    int i;

    pDWP = (DWP *) USER_HEAP_LIN_ADDR( hdwp );
    if (!pDWP) return FALSE;
    for (i = 0, winpos = pDWP->winPos; i < pDWP->actualCount; i++, winpos++)
    {
        if (!(res = USER_Driver.pSetWindowPos( winpos ))) break;
    }
    USER_HEAP_FREE( hdwp );
    return res;
}


/***********************************************************************
 *		TileChildWindows (USER.199)
 */
void WINAPI TileChildWindows16( HWND16 parent, WORD action )
{
    FIXME("(%04x, %d): stub\n", parent, action);
}

/***********************************************************************
 *		CascadeChildWindows (USER.198)
 */
void WINAPI CascadeChildWindows16( HWND16 parent, WORD action )
{
    FIXME("(%04x, %d): stub\n", parent, action);
}

/***********************************************************************
 *		SetProgmanWindow (USER32.@)
 */
HRESULT WINAPI SetProgmanWindow ( HWND hwnd )
{
	hGlobalProgmanWindow = hwnd;
	return hGlobalProgmanWindow;
}

/***********************************************************************
 *		GetProgmanWindow (USER32.@)
 */
HRESULT WINAPI GetProgmanWindow ( )
{
	return hGlobalProgmanWindow;
}

/***********************************************************************
 *		SetShellWindowEx (USER32.@)
 * hwndProgman =  Progman[Program Manager]
 *                |-> SHELLDLL_DefView
 * hwndListView = |   |-> SysListView32
 *                |   |   |-> tooltips_class32
 *                |   |
 *                |   |-> SysHeader32
 *                |   
 *                |-> ProxyTarget
 */
HRESULT WINAPI SetShellWindowEx ( HWND hwndProgman, HWND hwndListView )
{
	FIXME("0x%08x 0x%08x stub\n",hwndProgman ,hwndListView );
	hGlobalShellWindow = hwndProgman;
	return hGlobalShellWindow;

}

/***********************************************************************
 *		SetTaskmanWindow (USER32.@)
 * NOTES
 *   hwnd = MSTaskSwWClass 
 *          |-> SysTabControl32
 */
HRESULT WINAPI SetTaskmanWindow ( HWND hwnd )
{
	hGlobalTaskmanWindow = hwnd;
	return hGlobalTaskmanWindow;
}

/***********************************************************************
 *		GetTaskmanWindow (USER32.@)
 */
HRESULT WINAPI GetTaskmanWindow ( )
{	
	return hGlobalTaskmanWindow;
}
