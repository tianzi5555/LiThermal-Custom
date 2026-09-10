#include <my_main.h>

static MyCard cardCrosshair;
static lv_obj_t *ui_RollerColor = NULL;
static lv_obj_t *ui_SliderLength = NULL;
static lv_obj_t *ui_LabelLengthVal = NULL;
static lv_obj_t *ui_SliderThickness = NULL;
static lv_obj_t *ui_LabelThicknessVal = NULL;
static bool crosshair_settings_open = false;

static void color_roller_event(lv_event_t *e)
{
    globalSettings.crosshairColor = lv_roller_get_selected((lv_obj_t *)e->target);
    ui_center_display_updateStyle();
}

static void length_slider_event(lv_event_t *e)
{
    uint32_t v = lv_slider_get_value((lv_obj_t *)e->target);
    globalSettings.crosshairLength = v;
    if (lv_obj_is_valid(ui_LabelLengthVal))
        lv_label_set_text_fmt(ui_LabelLengthVal, "%d", v);
    ui_center_display_updateStyle();
}

static void thickness_slider_event(lv_event_t *e)
{
    uint32_t v = lv_slider_get_value((lv_obj_t *)e->target);
    globalSettings.crosshairThickness = v;
    if (lv_obj_is_valid(ui_LabelThicknessVal))
        lv_label_set_text_fmt(ui_LabelThicknessVal, "%d", v);
    ui_center_display_updateStyle();
}

void crosshair_settings_show()
{
    if (crosshair_settings_open)
        return;
    crosshair_settings_open = true;

    cardCrosshair.create(lv_layer_top(), 0, 20, 300, 200, LV_ALIGN_TOP_MID);
    lv_obj_set_style_pad_all(cardCrosshair.obj, 0, 0);
    lv_obj_set_style_bg_opa(cardCrosshair.obj, 220, 0);
    lv_obj_set_style_border_width(cardCrosshair.obj, 0, 0);
    lv_obj_clear_flag(cardCrosshair.obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(cardCrosshair.obj, &ui_font_chinese16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *lblColor = lv_label_create(cardCrosshair.obj);
    lv_obj_set_pos(lblColor, 10, 12);
    lv_label_set_text(lblColor, "COLOR");
    lv_obj_set_style_text_font(lblColor, &ui_font_chinese16, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_RollerColor = lv_roller_create(cardCrosshair.obj);
    lv_roller_set_options(ui_RollerColor, "WHITE\nGREEN\nRED\nYELLOW\nBLUE", LV_ROLLER_MODE_INFINITE);
    lv_roller_set_selected(ui_RollerColor, globalSettings.crosshairColor, LV_ANIM_OFF);
    lv_obj_set_size(ui_RollerColor, 130, 120);
    lv_obj_set_pos(ui_RollerColor, 150, 8);
    lv_obj_add_flag(ui_RollerColor, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(ui_RollerColor, color_roller_event, LV_EVENT_KEY, NULL);

    lv_obj_t *lblLen = lv_label_create(cardCrosshair.obj);
    lv_obj_set_pos(lblLen, 10, 145);
    lv_label_set_text(lblLen, "LEN");
    lv_obj_set_style_text_font(lblLen, &ui_font_chinese16, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_SliderLength = lv_slider_create(cardCrosshair.obj);
    lv_slider_set_range(ui_SliderLength, 6, 60);
    lv_slider_set_value(ui_SliderLength, globalSettings.crosshairLength, LV_ANIM_OFF);
    lv_obj_set_size(ui_SliderLength, 150, 16);
    lv_obj_set_pos(ui_SliderLength, 60, 145);
    lv_obj_add_flag(ui_SliderLength, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(ui_SliderLength, length_slider_event, LV_EVENT_VALUE_CHANGED, NULL);

    ui_LabelLengthVal = lv_label_create(cardCrosshair.obj);
    lv_obj_set_pos(ui_LabelLengthVal, 235, 143);
    lv_label_set_text_fmt(ui_LabelLengthVal, "%d", globalSettings.crosshairLength);
    lv_obj_set_style_text_font(ui_LabelLengthVal, &ui_font_chinese16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *lblWid = lv_label_create(cardCrosshair.obj);
    lv_obj_set_pos(lblWid, 10, 175);
    lv_label_set_text(lblWid, "WID");
    lv_obj_set_style_text_font(lblWid, &ui_font_chinese16, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_SliderThickness = lv_slider_create(cardCrosshair.obj);
    lv_slider_set_range(ui_SliderThickness, 1, 10);
    lv_slider_set_value(ui_SliderThickness, globalSettings.crosshairThickness, LV_ANIM_OFF);
    lv_obj_set_size(ui_SliderThickness, 150, 16);
    lv_obj_set_pos(ui_SliderThickness, 60, 175);
    lv_obj_add_flag(ui_SliderThickness, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(ui_SliderThickness, thickness_slider_event, LV_EVENT_VALUE_CHANGED, NULL);

    ui_LabelThicknessVal = lv_label_create(cardCrosshair.obj);
    lv_obj_set_pos(ui_LabelThicknessVal, 235, 173);
    lv_label_set_text_fmt(ui_LabelThicknessVal, "%d", globalSettings.crosshairThickness);
    lv_obj_set_style_text_font(ui_LabelThicknessVal, &ui_font_chinese16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_group_focus_obj(ui_RollerColor);
    cardCrosshair.show(CARD_ANIM_NONE);
}

void crosshair_settings_hide()
{
    if (!crosshair_settings_open)
        return;
    crosshair_settings_open = false;
    if (cardCrosshair.obj != NULL && lv_obj_is_valid(cardCrosshair.obj))
    {
        lv_obj_del(cardCrosshair.obj);
        cardCrosshair.obj = NULL;
    }
    ui_RollerColor = NULL;
    ui_SliderLength = NULL;
    ui_LabelLengthVal = NULL;
    ui_SliderThickness = NULL;
    ui_LabelThicknessVal = NULL;
    settings_save();
}

bool crosshair_settings_is_open()
{
    return crosshair_settings_open;
}
