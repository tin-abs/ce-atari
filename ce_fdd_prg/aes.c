#include <mint/sysbind.h>
#include <mint/osbind.h>
#include <mint/basepage.h>
#include <mint/ostruct.h>
#include <unistd.h>
#include <gem.h>
#include <mt_gem.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "acsi.h"
#include "main.h"
#include "hostmoddefs.h"
#include "keys.h"
#include "defs.h"
#include "CE_FDD.H"

typedef struct {
    OBJECT *tree;   // pointer to the tree
    int16_t xdial, ydial, wdial, hdial; // dimensions and position of dialog
} Dialog;

Dialog dialog;      // for easier passing to helper functions store everything needed in the struct
Dialog *cd;         // cd - pointer to current dialog, so we don't have to pass dialog pointer to functions

void unselectButton(int btnIdx)
{
    OBJECT *btn;
    rsrc_gaddr(R_OBJECT, btnIdx, &btn);    // get address of button

    if(!btn) {      // object not found? quit
        return;
    }

    btn->ob_state = btn->ob_state & (~OS_SELECTED);     // remove SELECTED flag

    objc_draw(cd->tree, btnIdx, 0, cd->xdial, cd->ydial, cd->wdial, cd->hdial);    // draw object tree - starting with the button
}

void setObjectString(int16_t objId, const char *newString)
{
    OBJECT *obj;
    rsrc_gaddr(R_OBJECT, objId, &obj);  // get address of object

    if(!obj) {                          // object not found? quit
        return;
    }

    int16_t ox, oy;
    objc_offset(cd->tree, objId, &ox, &oy);          // get current screen coordinates of object

    strcpy(obj->ob_spec.free_string, newString);    // copy in the string
    objc_draw(cd->tree, ROOT, MAX_DEPTH, ox, oy, obj->ob_width, obj->ob_height); // draw object tree, but clip only to text position and size
}

void showDialog(BYTE show)
{
    if(show) {  // on show
        form_center(cd->tree, &cd->xdial, &cd->ydial, &cd->wdial, &cd->hdial);       // center object
        form_dial(0, 0, 0, 0, 0, cd->xdial, cd->ydial, cd->wdial, cd->hdial);       // reserve screen space for dialog
        objc_draw(cd->tree, ROOT, MAX_DEPTH, cd->xdial, cd->ydial, cd->wdial, cd->hdial);  // draw object tree
    } else {    // on hide
        form_dial (3, 0, 0, 0, 0, cd->xdial, cd->ydial, cd->wdial, cd->hdial);      // release screen space
    }
}

void showErrorDialog(char *errorText)
{
    showDialog(FALSE);   // hide dialog

    char tmp[256];
    strcpy(tmp, "[3][");                // STOP icon
    strcat(tmp, errorText);             // error text
    strcat(tmp, "][ OK ]");             // OK button

    form_alert(1, tmp);                 // show alert dialog, 1st button as default one, with text and buttons in tmp[]

    showDialog(TRUE);   // show dialog
}

void showComErrorDialog(void)
{
    showErrorDialog("Error in CosmosEx communication!");
}
