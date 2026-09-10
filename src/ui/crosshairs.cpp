#include <my_main.h>

lv_obj_t *crosshairs_max_obj = NULL;
lv_obj_t *crosshairs_min_obj = NULL;
lv_obj_t *crosshairs_max = NULL;
lv_obj_t *crosshairs_min = NULL;
lv_obj_t *crosshairs_label_max = NULL;
lv_obj_t *crosshairs_label_min = NULL;

extern "C" const lv_img_dsc_t crosshairs;

// 把全画面归一化坐标(0..1)映射到当前数码变焦后的屏幕坐标
static bool map_full_to_screen(float normX, float normY, float *screenX, float *screenY)
{
    int srcW = g_last_crop[4];
    int srcH = g_last_crop[5];
    int cropX = g_last_crop[0];
    int cropY = g_last_crop[1];
    int cropW = g_last_crop[2];
    int cropH = g_last_crop[3];

    if (srcW <= 0 || srcH <= 0 || cropW <= 0 || cropH <= 0)
    {
        *screenX = normX * 320.0f;
        *screenY = normY * 240.0f;
        return true;
    }

    float fullX = normX * srcW;
    float fullY = normY * srcH;
    if (fullX < cropX || fullX > cropX + cropW || fullY < cropY || fullY > cropY + cropH)
        return false;

    *screenX = (fullX - cropX) * 320.0f / cropW;
    *screenY = (fullY - cropY) * 240.0f / cropH;
    return true;
}

void ui_crosshairs_updateVisibility()
{
    if (globalSettings.enableMaxValueDisplay)
    {
        lv_obj_clear_flag(crosshairs_max_obj, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(crosshairs_max_obj, LV_OBJ_FLAG_HIDDEN);
    }
    if (globalSettings.enableMinValueDisplay)
    {
        lv_obj_clear_flag(crosshairs_min_obj, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(crosshairs_min_obj, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(crosshairs_max_obj);
    lv_obj_move_foreground(crosshairs_min_obj);
}

static void crosshair_anim_move(lv_obj_t *obj, lv_coord_t x, lv_coord_t y)
{
    lv_anim_t a;
    int16_t p;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_time(&a, 40);
    lv_anim_set_delay(&a, 0);
    p = lv_obj_get_style_x(obj, 0);
    if (p != x)
    {
        lv_anim_set_values(&a, p, x);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
        lv_anim_start(&a);
    }
    p = lv_obj_get_style_y(obj, 0);
    if (p != y)
    {
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_set_values(&a, p, y);
        lv_anim_start(&a);
    }
}

static bool hidden_by_view = false;
void ui_crosshairs_updatePos()
{
    float x, y;
    char buffer[16];
    if (hidden_by_view == true)
    {
        if (current_mode == MODE_MAINPAGE || current_mode == MODE_CAMERA_SETTINGS)
        {
            hidden_by_view = false;
            ui_crosshairs_updateVisibility();
        }
        else
        {
            return;
        }
    }
    if (hidden_by_view == false && current_mode != MODE_MAINPAGE && current_mode != MODE_CAMERA_SETTINGS)
    {
        hidden_by_view = true;
        lv_obj_add_flag(crosshairs_max_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(crosshairs_min_obj, LV_OBJ_FLAG_HIDDEN);
    }
    if (globalSettings.enableMaxValueDisplay)
    {
        if (map_full_to_screen(cameraUtils.lastResult.MaxTemperaturePoint.positionX,
                               cameraUtils.lastResult.MaxTemperaturePoint.positionY, &x, &y))
        {
            lv_obj_clear_flag(crosshairs_max_obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(crosshairs_max_obj);
            x -= 7;
            y -= 7;
            crosshair_anim_move(crosshairs_max_obj, x, y);
            sprintf(buffer, "%.1f", cameraUtils.lastResult.maxTemperature);
            lv_label_set_text(crosshairs_label_max, buffer);
        }
        else
        {
            lv_obj_add_flag(crosshairs_max_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (globalSettings.enableMinValueDisplay)
    {
        if (map_full_to_screen(cameraUtils.lastResult.MinTemperaturePoint.positionX,
                               cameraUtils.lastResult.MinTemperaturePoint.positionY, &x, &y))
        {
            lv_obj_clear_flag(crosshairs_min_obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(crosshairs_min_obj);
            x -= 7;
            y -= 7;
            crosshair_anim_move(crosshairs_min_obj, x, y);
            sprintf(buffer, "%.1f", cameraUtils.lastResult.minTemperature);
            lv_label_set_text(crosshairs_label_min, buffer);
        }
        else
        {
            lv_obj_add_flag(crosshairs_min_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_crosshairs_create()
{
    crosshairs_max_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(crosshairs_max_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(crosshairs_max_obj, 0, 0);
    lv_obj_set_style_bg_opa(crosshairs_max_obj, 0, 0);
    lv_obj_set_style_border_width(crosshairs_max_obj, 0, 0);
    lv_obj_set_style_radius(crosshairs_max_obj, 0, 0);
    lv_obj_add_flag(crosshairs_max_obj, LV_OBJ_FLAG_HIDDEN);

    crosshairs_max = lv_img_create(crosshairs_max_obj);
    lv_img_set_src(crosshairs_max, &crosshairs);
    crosshairs_label_max = lv_label_create(crosshairs_max_obj);
    lv_obj_set_style_bg_color(crosshairs_label_max, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(crosshairs_label_max, LV_OPA_50, 0);
    lv_obj_set_style_radius(crosshairs_label_max, 3, 0);
    lv_obj_align_to(crosshairs_label_max, crosshairs_max, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

    crosshairs_min_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(crosshairs_min_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(crosshairs_min_obj, 0, 0);
    lv_obj_set_style_bg_opa(crosshairs_min_obj, 0, 0);
    lv_obj_set_style_border_width(crosshairs_min_obj, 0, 0);
    lv_obj_set_style_radius(crosshairs_min_obj, 0, 0);
    lv_obj_add_flag(crosshairs_min_obj, LV_OBJ_FLAG_HIDDEN);

    crosshairs_min = lv_img_create(crosshairs_min_obj);
    lv_img_set_src(crosshairs_min, &crosshairs);
    crosshairs_label_min = lv_label_create(crosshairs_min_obj);
    lv_obj_set_style_bg_color(crosshairs_label_min, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(crosshairs_label_min, LV_OPA_50, 0);
    lv_obj_set_style_radius(crosshairs_label_min, 3, 0);
    lv_obj_align_to(crosshairs_label_min, crosshairs_min, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
}

// ===================== 本地中心温度/准星显示 =====================
static lv_obj_t *center_cross_obj = NULL;      // 绿色圆形准星容器
static lv_obj_t *center_pure_cross_obj = NULL; // 纯十字准星容器
static lv_obj_t *center_label = NULL;

static void ui_center_display_updateVisibility();

void ui_center_display_create()
{
    if (center_cross_obj != NULL && lv_obj_is_valid(center_cross_obj))
        return;

    center_cross_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(center_cross_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(center_cross_obj, 0, 0);
    lv_obj_set_style_bg_opa(center_cross_obj, 0, 0);
    lv_obj_set_style_border_width(center_cross_obj, 0, 0);
    lv_obj_set_style_radius(center_cross_obj, 0, 0);
    lv_obj_clear_flag(center_cross_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(center_cross_obj);
    lv_obj_t *img = lv_img_create(center_cross_obj);
    lv_img_set_src(img, &crosshairs);

    center_pure_cross_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(center_pure_cross_obj, 20, 20);
    lv_obj_set_style_pad_all(center_pure_cross_obj, 0, 0);
    lv_obj_set_style_bg_opa(center_pure_cross_obj, 0, 0);
    lv_obj_set_style_border_width(center_pure_cross_obj, 0, 0);
    lv_obj_set_style_radius(center_pure_cross_obj, 0, 0);
    lv_obj_clear_flag(center_pure_cross_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(center_pure_cross_obj);
    lv_obj_t *bar_h = lv_obj_create(center_pure_cross_obj);
    lv_obj_set_size(bar_h, 20, 2);
    lv_obj_center(bar_h);
    lv_obj_set_style_bg_color(bar_h, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar_h, LV_OPA_70, 0);
    lv_obj_set_style_border_width(bar_h, 0, 0);
    lv_obj_set_style_radius(bar_h, 0, 0);
    lv_obj_t *bar_v = lv_obj_create(center_pure_cross_obj);
    lv_obj_set_size(bar_v, 2, 20);
    lv_obj_center(bar_v);
    lv_obj_set_style_bg_color(bar_v, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar_v, LV_OPA_70, 0);
    lv_obj_set_style_border_width(bar_v, 0, 0);
    lv_obj_set_style_radius(bar_v, 0, 0);

    center_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_bg_color(center_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(center_label, LV_OPA_50, 0);
    lv_obj_set_style_radius(center_label, 3, 0);
    lv_obj_set_style_text_font(center_label, &ui_font_chinese16, 0);
    lv_obj_align(center_label, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_label_set_text(center_label, "中心 --.-");

    ui_center_display_updateVisibility();
}

void ui_center_display_update()
{
    if (center_label != NULL && lv_obj_is_valid(center_label))
    {
        char buf[32];
        sprintf(buf, "中心 %.1f", cameraUtils.lastCenterTemperature);
        lv_label_set_text(center_label, buf);
    }
}

static void ui_center_display_updateVisibility()
{
    bool show = globalSettings.enableCenterValueDisplay &&
                (current_mode == MODE_MAINPAGE || current_mode == MODE_CAMERA_SETTINGS);

    if (center_cross_obj != NULL && lv_obj_is_valid(center_cross_obj))
    {
        if (show && !globalSettings.use4117Cursors)
        {
            lv_obj_clear_flag(center_cross_obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(center_cross_obj);
        }
        else
        {
            lv_obj_add_flag(center_cross_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (center_pure_cross_obj != NULL && lv_obj_is_valid(center_pure_cross_obj))
    {
        if (show && globalSettings.use4117Cursors)
        {
            lv_obj_clear_flag(center_pure_cross_obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(center_pure_cross_obj);
        }
        else
        {
            lv_obj_add_flag(center_pure_cross_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (center_label != NULL && lv_obj_is_valid(center_label))
    {
        if (show)
        {
            lv_obj_clear_flag(center_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(center_label);
        }
        else
        {
            lv_obj_add_flag(center_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
