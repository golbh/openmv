/*
 * This file is part of the OpenMV project.
 * Copyright (c) 2013/2014 Ibrahim Abdelkader <i.abdalkader@gmail.com>
 * This work is licensed under the MIT license, see the file LICENSE for details.
 *
 * Image Python module.
 *
 */
#include "mp.h"
#include "imlib.h"
#include "array.h"
#include "sensor.h"
#include "ff.h"
#include "xalloc.h"
#include "arm_math.h"
#include "py_assert.h"
#include "py_helper.h"
#include "py_image.h"
#include "omv_boardconfig.h"

#define JPEG_INIT_BUF   (5*1024)

extern sensor_t sensor;
static const mp_obj_type_t py_cascade_type;
static const mp_obj_type_t py_image_type;

extern const char *ffs_strerror(FRESULT res);

/* Haar Cascade */
typedef struct _py_cascade_obj_t {
    mp_obj_base_t base;
    struct cascade _cobj;
} py_cascade_obj_t;

void *py_cascade_cobj(mp_obj_t cascade)
{
    PY_ASSERT_TYPE(cascade, &py_cascade_type);
    return &((py_cascade_obj_t *)cascade)->_cobj;
}

static void py_cascade_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    py_cascade_obj_t *self = self_in;
    /* print some info */
    mp_printf(print, "width:%d height:%d n_stages:%d n_features:%d n_rectangles:%d\n", self->_cobj.window.w,
          self->_cobj.window.h, self->_cobj.n_stages, self->_cobj.n_features, self->_cobj.n_rectangles);
}

static const mp_obj_type_t py_cascade_type = {
    { &mp_type_type },
    .name  = MP_QSTR_Cascade,
    .print = py_cascade_print,
};

/* Keypoints object */
typedef struct _py_kp_obj_t {
    mp_obj_base_t base;
    array_t *kpts;
    int threshold;
    bool normalized;
} py_kp_obj_t;

static void py_kp_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    py_kp_obj_t *self = self_in;
    mp_printf(print, "size:%d threshold:%d normalized:%d\n", array_length(self->kpts), self->threshold, self->normalized);
}

static const mp_obj_type_t py_kp_type = {
    { &mp_type_type },
    .name  = MP_QSTR_kp_desc,
    .print = py_kp_print,
};

/* LBP descriptor */
typedef struct _py_lbp_obj_t {
    mp_obj_base_t base;
    uint8_t *hist;
} py_lbp_obj_t;

static void py_lbp_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    mp_printf(print, "<lbp descriptor>");
}

static const mp_obj_type_t py_lbp_type = {
    { &mp_type_type },
    .name  = MP_QSTR_lbp_desc,
    .print = py_lbp_print,
};

/* Image */
typedef struct _py_image_obj_t {
    mp_obj_base_t base;
    struct image _cobj;
} py_image_obj_t;

void *py_image_cobj(mp_obj_t image)
{
    PY_ASSERT_TYPE(image, &py_image_type);
    return &((py_image_obj_t *)image)->_cobj;
}

static void py_image_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    py_image_obj_t *self = self_in;
    mp_printf(print, "<image width:%d height:%d bpp:%d>", self->_cobj.w, self->_cobj.h, self->_cobj.bpp);
}

static mp_int_t py_image_get_buffer(mp_obj_t self_in, mp_buffer_info_t *bufinfo, mp_uint_t flags) {
    image_t *image = py_image_cobj(self_in);

    if (flags == MP_BUFFER_READ) {
        bufinfo->buf = (void*)image->pixels;
        if (image->bpp > 2) { //JPEG
            bufinfo->len = image->bpp;
        } else {
            bufinfo->len = image->w*image->h*image->bpp;
        }
        bufinfo->typecode = 'b';
        return 0;
    } else {
        // disable write for now
        bufinfo->buf = NULL;
        bufinfo->len = 0;
        bufinfo->typecode = -1;
        return 1;
    }
}

static mp_obj_t py_image_subscr(mp_obj_t self_in, mp_obj_t index_in, mp_obj_t value) {
    py_image_obj_t *o = self_in;
    image_t *image = py_image_cobj(self_in);

    if (value == MP_OBJ_NULL) {
        // delete
        return MP_OBJ_NULL; // op not supported
    } else if (value == MP_OBJ_SENTINEL) {
        // load
        mp_uint_t pixel;
        mp_uint_t index = mp_get_index(o->base.type, image->w*image->h, index_in, false);
        switch (image->bpp) {
            case 1:
                pixel = image->pixels[index];
                break;
            case 2:
                pixel = image->pixels[index*2]<<8 | image->pixels[index*2+1];
                break;
            default:
                return MP_OBJ_NULL; // op not supported
        }
        return mp_obj_new_int(pixel);
    } else {
        // store
        return mp_const_none;
    }
}

static mp_obj_t py_image_save(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    const char *path = mp_obj_str_get_str(args[1]);

    rectangle_t roi;
    py_helper_lookup_rectangle(kw_args, arg_img, &roi);

    imlib_save_image(arg_img, path, &roi);
    return mp_const_true;
}

static mp_obj_t py_image_width(mp_obj_t img_obj)
{
    image_t *arg_img = py_image_cobj(img_obj);
    return mp_obj_new_int(arg_img->w);
}

static mp_obj_t py_image_height(mp_obj_t img_obj)
{
    image_t *arg_img = py_image_cobj(img_obj);
    return mp_obj_new_int(arg_img->h);
}

static mp_obj_t py_image_format(mp_obj_t img_obj)
{
    image_t *arg_img = py_image_cobj(img_obj);
    if (IM_IS_GS(arg_img)) {
        return mp_obj_new_int(PIXFORMAT_GRAYSCALE);
    } else if (IM_IS_RGB565(arg_img)) {
        return mp_obj_new_int(PIXFORMAT_RGB565);
    } else {
        return mp_obj_new_int(PIXFORMAT_JPEG);
    }
}

static mp_obj_t py_image_size(mp_obj_t img_obj)
{
    image_t *arg_img = py_image_cobj(img_obj);
    if (IM_IS_JPEG(arg_img)) {
        return mp_obj_new_int(arg_img->bpp);
    } else {
        return mp_obj_new_int(arg_img->w * arg_img->h * arg_img->bpp);
    }
}

static mp_obj_t py_image_get_pixel(mp_obj_t img_obj, mp_obj_t x_obj, mp_obj_t y_obj)
{
    image_t *arg_img = py_image_cobj(img_obj);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    int arg_x = mp_obj_get_int(x_obj);
    int arg_y = mp_obj_get_int(y_obj);
    if ((!IM_X_INSIDE(arg_img, arg_x)) || (!IM_Y_INSIDE(arg_img, arg_y))) {
        return mp_const_none;
    }

    if (IM_IS_GS(arg_img)) {
        return mp_obj_new_int(IM_GET_GS_PIXEL(arg_img, arg_x, arg_y));
    } else {
        uint16_t pixel = IM_GET_RGB565_PIXEL(arg_img, arg_x, arg_y);
        mp_obj_t pixel_tuple[3];
        pixel_tuple[0] = mp_obj_new_int(IM_R528(IM_R565(pixel)));
        pixel_tuple[1] = mp_obj_new_int(IM_G628(IM_G565(pixel)));
        pixel_tuple[2] = mp_obj_new_int(IM_B528(IM_B565(pixel)));
        return mp_obj_new_tuple(3, pixel_tuple);
    }
}

static mp_obj_t py_image_set_pixel(uint n_args, const mp_obj_t *args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    int arg_x = mp_obj_get_int(args[1]);
    int arg_y = mp_obj_get_int(args[2]);
    if ((!IM_X_INSIDE(arg_img, arg_x)) || (!IM_Y_INSIDE(arg_img, arg_y))) {
        return mp_const_none;
    }

    if (IM_IS_GS(arg_img)) {
        IM_SET_GS_PIXEL(arg_img, arg_x, arg_y, mp_obj_get_int(args[3]));
    } else {
        mp_obj_t *arg_color;
        mp_obj_get_array_fixed_n(args[3], 3, &arg_color);
        int red = IM_R825(mp_obj_get_int(arg_color[0]));
        int green = IM_G826(mp_obj_get_int(arg_color[1]));
        int blue = IM_B825(mp_obj_get_int(arg_color[2]));
        IM_SET_RGB565_PIXEL(arg_img, arg_x, arg_y, IM_RGB565(red, green, blue));
    }
    return mp_const_none;
}

static mp_obj_t py_image_draw_line(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    mp_obj_t *arg_vec;
    mp_obj_get_array_fixed_n(args[1], 4, &arg_vec);

    int arg_x0 = mp_obj_get_int(arg_vec[0]);
    int arg_y0 = mp_obj_get_int(arg_vec[1]);
    int arg_x1 = mp_obj_get_int(arg_vec[2]);
    int arg_y1 = mp_obj_get_int(arg_vec[3]);
    int arg_c  = py_helper_lookup_color(kw_args, -1); // white

    imlib_draw_line(arg_img, arg_x0, arg_y0, arg_x1, arg_y1, arg_c);
    return mp_const_none;
}

static mp_obj_t py_image_draw_rectangle(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    mp_obj_t *arg_vec;
    mp_obj_get_array_fixed_n(args[1], 4, &arg_vec);

    int arg_rx = mp_obj_get_int(arg_vec[0]);
    int arg_ry = mp_obj_get_int(arg_vec[1]);
    int arg_rw = mp_obj_get_int(arg_vec[2]);
    int arg_rh = mp_obj_get_int(arg_vec[3]);
    int arg_c  = py_helper_lookup_color(kw_args, -1); // white

    imlib_draw_rectangle(arg_img, arg_rx, arg_ry, arg_rw, arg_rh, arg_c);
    return mp_const_none;
}

static mp_obj_t py_image_draw_circle(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    int arg_cx = mp_obj_get_int(args[1]);
    int arg_cy = mp_obj_get_int(args[2]);
    int arg_r  = mp_obj_get_int(args[3]);
    int arg_c  = py_helper_lookup_color(kw_args, -1); // white

    imlib_draw_circle(arg_img, arg_cx, arg_cy, arg_r, arg_c);
    return mp_const_none;
}

static mp_obj_t py_image_draw_string(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    int arg_x_off       = mp_obj_get_int(args[1]);
    int arg_y_off       = mp_obj_get_int(args[2]);
    const char *arg_str = mp_obj_str_get_str(args[3]);
    int arg_c           = py_helper_lookup_color(kw_args, -1); // white

    imlib_draw_string(arg_img, arg_x_off, arg_y_off, arg_str, arg_c);
    return mp_const_none;
}

static mp_obj_t py_image_draw_cross(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    int arg_x = mp_obj_get_int(args[1]);
    int arg_y = mp_obj_get_int(args[2]);
    int arg_c = py_helper_lookup_color(kw_args, -1); // white
    int arg_s = py_helper_lookup_int(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_size), 5);

    imlib_draw_line(arg_img, arg_x-arg_s, arg_y      , arg_x+arg_s, arg_y      , arg_c);
    imlib_draw_line(arg_img, arg_x      , arg_y-arg_s, arg_x      , arg_y+arg_s, arg_c);
    return mp_const_none;
}

static mp_obj_t py_image_draw_keypoints(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    int arg_c = py_helper_lookup_color(kw_args, -1); // white
    int arg_s = py_helper_lookup_int(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_size), 10);

    if (MP_OBJ_IS_TYPE(args[1],&mp_type_tuple)||(MP_OBJ_IS_TYPE(args[1],&mp_type_list))) {
        mp_uint_t arg_vec_len;
        mp_obj_t *arg_vec;
        mp_obj_get_array(args[1], &arg_vec_len, &arg_vec);
        if (!arg_vec_len) return mp_const_none;
        for (int i=0; i<arg_vec_len; i++) {
            mp_obj_t *arg_keypoint;
            mp_obj_get_array_fixed_n(arg_vec[i], 3, &arg_keypoint);
            int x = mp_obj_get_int(arg_keypoint[0]);
            int y = mp_obj_get_int(arg_keypoint[1]);
            float angle = mp_obj_get_float(arg_keypoint[2]);
            float co = arm_cos_f32(angle);
            float si = arm_sin_f32(angle);
            imlib_draw_line(arg_img, x, y, x+(co*arg_s), y+(si*arg_s), arg_c);
            imlib_draw_circle(arg_img, x, y, (arg_s-2)/2, arg_c);
        }
    } else {
        py_kp_obj_t *kpts_obj = ((py_kp_obj_t*)args[1]);
        PY_ASSERT_TYPE(kpts_obj, &py_kp_type);

        for (int i=0; i<array_length(kpts_obj->kpts); i++) {
            kp_t *kp = array_at(kpts_obj->kpts, i);
            imlib_draw_circle(arg_img, kp->x, kp->y, (arg_s-2)/2, arg_c);
        }
    }
    return mp_const_none;
}

static mp_obj_t py_image_binary(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    mp_uint_t arg_t_len;
    mp_obj_t *arg_t;
    mp_obj_get_array(args[1], &arg_t_len, &arg_t);
    if (!arg_t_len) return mp_const_none;

    simple_color_t l_t[arg_t_len], u_t[arg_t_len];
    if (IM_IS_GS(arg_img)) {
        for (int i=0; i<arg_t_len; i++) {
            mp_obj_t *temp;
            mp_obj_get_array_fixed_n(arg_t[i], 2, &temp);
            int lo = mp_obj_get_int(temp[0]);
            int hi = mp_obj_get_int(temp[1]);
            // Swap ranges if they are wrong.
            l_t[i].G = IM_MIN(lo, hi);
            u_t[i].G = IM_MAX(lo, hi);
        }
    } else {
        for (int i=0; i<arg_t_len; i++) {
            mp_obj_t *temp;
            mp_obj_get_array_fixed_n(arg_t[i], 6, &temp);
            int l_lo = mp_obj_get_int(temp[0]);
            int l_hi = mp_obj_get_int(temp[1]);
            int a_lo = mp_obj_get_int(temp[2]);
            int a_hi = mp_obj_get_int(temp[3]);
            int b_lo = mp_obj_get_int(temp[4]);
            int b_hi = mp_obj_get_int(temp[5]);
            // Swap ranges if they are wrong.
            l_t[i].L = IM_MIN(l_lo, l_hi);
            u_t[i].L = IM_MAX(l_lo, l_hi);
            l_t[i].A = IM_MIN(a_lo, a_hi);
            u_t[i].A = IM_MAX(a_lo, a_hi);
            l_t[i].B = IM_MIN(b_lo, b_hi);
            u_t[i].B = IM_MAX(b_lo, b_hi);
        }
    }

    int arg_invert = py_helper_lookup_int(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_invert), 0);
    imlib_binary(arg_img, arg_t_len, l_t, u_t, arg_invert ? 1 : 0);
    return mp_const_none;
}

static mp_obj_t py_image_invert(mp_obj_t img_obj)
{
    image_t *arg_img = py_image_cobj(img_obj);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    imlib_invert(arg_img);
    return mp_const_none;
}

static mp_obj_t py_image_and(mp_obj_t img_obj, mp_obj_t other_obj)
{
    image_t *arg_img = py_image_cobj(img_obj);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    if (MP_OBJ_IS_STR(other_obj)) {
        imlib_and(arg_img, mp_obj_str_get_str(other_obj), NULL);
    } else {
        image_t *arg_other = py_image_cobj(other_obj);
        imlib_and(arg_img, NULL, arg_other);
    }
    return mp_const_none;
}

static mp_obj_t py_image_nand(mp_obj_t img_obj, mp_obj_t other_obj)
{
    image_t *arg_img = py_image_cobj(img_obj);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    if (MP_OBJ_IS_STR(other_obj)) {
        imlib_nand(arg_img, mp_obj_str_get_str(other_obj), NULL);
    } else {
        image_t *arg_other = py_image_cobj(other_obj);
        imlib_nand(arg_img, NULL, arg_other);
    }
    return mp_const_none;
}

static mp_obj_t py_image_or(mp_obj_t img_obj, mp_obj_t other_obj)
{
    image_t *arg_img = py_image_cobj(img_obj);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    if (MP_OBJ_IS_STR(other_obj)) {
        imlib_or(arg_img, mp_obj_str_get_str(other_obj), NULL);
    } else {
        image_t *arg_other = py_image_cobj(other_obj);
        imlib_or(arg_img, NULL, arg_other);
    }
    return mp_const_none;
}

static mp_obj_t py_image_nor(mp_obj_t img_obj, mp_obj_t other_obj)
{
    image_t *arg_img = py_image_cobj(img_obj);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    if (MP_OBJ_IS_STR(other_obj)) {
        imlib_nor(arg_img, mp_obj_str_get_str(other_obj), NULL);
    } else {
        image_t *arg_other = py_image_cobj(other_obj);
        imlib_nor(arg_img, NULL, arg_other);
    }
    return mp_const_none;
}

static mp_obj_t py_image_xor(mp_obj_t img_obj, mp_obj_t other_obj)
{
    image_t *arg_img = py_image_cobj(img_obj);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    if (MP_OBJ_IS_STR(other_obj)) {
        imlib_xor(arg_img, mp_obj_str_get_str(other_obj), NULL);
    } else {
        image_t *arg_other = py_image_cobj(other_obj);
        imlib_xor(arg_img, NULL, arg_other);
    }
    return mp_const_none;
}

static mp_obj_t py_image_xnor(mp_obj_t img_obj, mp_obj_t other_obj)
{
    image_t *arg_img = py_image_cobj(img_obj);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    if (MP_OBJ_IS_STR(other_obj)) {
        imlib_xnor(arg_img, mp_obj_str_get_str(other_obj), NULL);
    } else {
        image_t *arg_other = py_image_cobj(other_obj);
        imlib_xnor(arg_img, NULL, arg_other);
    }
    return mp_const_none;
}

static mp_obj_t py_image_pixels(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    rectangle_t arg_r;
    py_helper_lookup_rectangle(kw_args, arg_img, &arg_r);
    return mp_obj_new_int(imlib_pixels(arg_img, &arg_r));
}

static mp_obj_t py_image_centroid(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    rectangle_t arg_r;
    py_helper_lookup_rectangle(kw_args, arg_img, &arg_r);
    int x, y;
    int sum = imlib_centroid(arg_img, &x, &y, &arg_r);

    return mp_obj_new_tuple(3, (mp_obj_t[3])
            {mp_obj_new_int(sum), mp_obj_new_int(x), mp_obj_new_int(y)});
}

static mp_obj_t py_image_orientation_radians(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    rectangle_t arg_r;
    py_helper_lookup_rectangle(kw_args, arg_img, &arg_r);
    int sum, x, y;
    float o = imlib_orientation_radians(arg_img, &sum, &x, &y, &arg_r);

    return mp_obj_new_tuple(4, (mp_obj_t[4])
            {mp_obj_new_int(sum), mp_obj_new_int(x), mp_obj_new_int(y), mp_obj_new_float(o)});
}

static mp_obj_t py_image_orientation_degrees(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    rectangle_t arg_r;
    py_helper_lookup_rectangle(kw_args, arg_img, &arg_r);
    int sum, x, y;
    float o = imlib_orientation_degrees(arg_img, &sum, &x, &y, &arg_r);

    return mp_obj_new_tuple(4, (mp_obj_t[4])
            {mp_obj_new_int(sum), mp_obj_new_int(x), mp_obj_new_int(y), mp_obj_new_float(o)});
}

static mp_obj_t py_image_erode(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    int ksize = mp_obj_get_int(args[1]);
    PY_ASSERT_TRUE_MSG(ksize >= 0, "Kernel Size must be >= 0");
    imlib_erode(arg_img, ksize,
            py_helper_lookup_int(kw_args,
            MP_OBJ_NEW_QSTR(MP_QSTR_threshold), ((ksize*2)+1)*((ksize*2)+1)));
    return mp_const_none;
}

static mp_obj_t py_image_dilate(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    image_t *arg_img = py_image_cobj(args[0]);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    int ksize = mp_obj_get_int(args[1]);
    PY_ASSERT_TRUE_MSG(ksize >= 0, "Kernel Size must be >= 0");
    imlib_dilate(arg_img, ksize,
            py_helper_lookup_int(kw_args,
            MP_OBJ_NEW_QSTR(MP_QSTR_threshold), 1));
    return mp_const_none;
}

static mp_obj_t py_image_negate(mp_obj_t img_obj)
{
    image_t *arg_img = py_image_cobj(img_obj);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    imlib_negate(arg_img);
    return mp_const_none;
}

static mp_obj_t py_image_difference(mp_obj_t img_obj, mp_obj_t other_obj)
{
    image_t *arg_img = py_image_cobj(img_obj);
    PY_ASSERT_FALSE_MSG(IM_IS_JPEG(arg_img),
            "Operation not supported on JPEG");

    if (MP_OBJ_IS_STR(other_obj)) {
        imlib_difference(arg_img, mp_obj_str_get_str(other_obj), NULL);
    } else {
        image_t *arg_other = py_image_cobj(other_obj);
        imlib_difference(arg_img, NULL, arg_other);
    }
    return mp_const_none;
}

static mp_obj_t py_image_scale(mp_obj_t image_obj, mp_obj_t size_obj)
{
    int w,h;
    mp_obj_t *array;
    image_t *src_image = NULL;

    /* get C image pointer */
    src_image = py_image_cobj(image_obj);

    /* get x,y */
    mp_obj_get_array_fixed_n(size_obj, 2, &array);
    w = mp_obj_get_int(array[0]);
    h = mp_obj_get_int(array[1]);

    image_t dst_image = {
        .w=w,
        .h=h,
        .bpp=src_image->bpp,
        .pixels=xalloc(w*h*src_image->bpp)
    };

    imlib_scale(src_image, &dst_image, INTERP_BILINEAR);

    *src_image = dst_image;
    return image_obj;
}

static mp_obj_t py_image_scaled(mp_obj_t image_obj, mp_obj_t size_obj)
{
    int w,h;
    mp_obj_t *array;
    image_t *src_image = NULL;

    /* get C image pointer */
    src_image = py_image_cobj(image_obj);

    /* get x,y */
    mp_obj_get_array_fixed_n(size_obj, 2, &array);
    w = mp_obj_get_int(array[0]);
    h = mp_obj_get_int(array[1]);

    image_t dst_image = {
        .w=w,
        .h=h,
        .bpp=src_image->bpp,
        .pixels=xalloc(w*h*src_image->bpp)
    };

    imlib_scale(src_image, &dst_image, INTERP_NEAREST);

    return py_image_from_struct(&dst_image);
}

static mp_obj_t py_image_subimg(mp_obj_t image_obj, mp_obj_t subimg_obj)
{
    rectangle_t r;
    image_t *image;
    mp_obj_t *array;

    /* image pointer */
    image = py_image_cobj(image_obj);

    /* sub image */
    mp_obj_get_array_fixed_n(subimg_obj, 4, &array);
    r.x = mp_obj_get_int(array[0]);
    r.y = mp_obj_get_int(array[1]);
    r.w = mp_obj_get_int(array[2]);
    r.h = mp_obj_get_int(array[3]);

    image_t subimg = {
        .w=r.w,
        .h=r.h,
        .bpp=image->bpp,
        .pixels=xalloc(r.w*r.h*image->bpp)
    };

    imlib_subimage(image, &subimg, r.x, r.y);

    return py_image_from_struct(&subimg);
}

static mp_obj_t py_image_blit(mp_obj_t dst_image_obj, mp_obj_t src_image_obj, mp_obj_t offset_obj)
{
    int x,y;
    image_t *src_image = NULL;
    image_t *dst_image = NULL;

    /* get C image pointer */
    src_image = py_image_cobj(src_image_obj);
    dst_image = py_image_cobj(dst_image_obj);

    /* get x,y */
    mp_obj_t *array;
    mp_obj_get_array_fixed_n(offset_obj, 2, &array);
    x = mp_obj_get_int(array[0]);
    y = mp_obj_get_int(array[1]);

    if ((src_image->w+x)>dst_image->w ||
        (src_image->h+y)>dst_image->h) {
        printf("src image > dst image\n");
        return mp_const_none;
    }

    imlib_blit(src_image, dst_image, x, y);
    return mp_const_none;
}

static mp_obj_t py_image_blend(mp_obj_t dst_image_obj,
        mp_obj_t src_image_obj, mp_obj_t param_obj)
{
    int x,y;
    float alpha;
    image_t *src_image = NULL;
    image_t *dst_image = NULL;

    /* get C image pointer */
    src_image = py_image_cobj(src_image_obj);
    dst_image = py_image_cobj(dst_image_obj);

    /* get x,y,alpha */
    mp_obj_t *array;
    mp_obj_get_array_fixed_n(param_obj, 3, &array);
    x = mp_obj_get_int(array[0]);
    y = mp_obj_get_int(array[1]);
    alpha = mp_obj_get_float(array[2]);

    if ((src_image->w+x)>dst_image->w ||
        (src_image->h+y)>dst_image->h) {
        printf("src image > dst image\n");
        return mp_const_none;
    }

    imlib_blend(src_image, dst_image, x, y, (uint8_t)(alpha*256));
    return mp_const_none;
}

static mp_obj_t py_image_histeq(mp_obj_t image_obj)
{
    struct image *image;
    /* get image pointer */
    image = (struct image*) py_image_cobj(image_obj);

    /* sanity checks */
    PY_ASSERT_TRUE_MSG(image->bpp == 1,
            "This function is only supported on GRAYSCALE images");

    imlib_histeq(image);
    return mp_const_none;
}

static mp_obj_t py_image_median(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    // Read args
    image_t *image = py_image_cobj(args[0]);
    int ksize = py_helper_lookup_int(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_size), 1);

    // Call median filter
    imlib_median_filter(image, ksize);
    return mp_const_none;
}

static mp_obj_t py_image_threshold(mp_obj_t image_obj, mp_obj_t color_list_obj, mp_obj_t threshold)
{
    color_t *color;
    image_t *image;

    /* sanity checks */
    PY_ASSERT_TRUE_MSG(sensor.pixformat == PIXFORMAT_RGB565,
            "This function is only supported on RGB565 images");

    PY_ASSERT_TRUE_MSG(sensor.framesize <= OMV_MAX_BLOB_FRAME,
            "This function is only supported on "OMV_MAX_BLOB_FRAME_STR" and smaller frames");

    /* read arguments */
    image = py_image_cobj(image_obj);
    int thresh = mp_obj_get_int(threshold);

    /* returned image */
    image_t bimage = {
        .w=image->w,
        .h=image->h,
        .bpp=1,
        .pixels=image->data+(image->w*image->h*image->bpp)
    };

    /* copy color list */
    uint len;
    mp_obj_t *color_arr;
    mp_obj_get_array(color_list_obj, &len, &color_arr);

    color = xalloc(len*sizeof*color);

    for (int i=0; i<len; i++) {
        mp_obj_t *color_obj;
        mp_obj_get_array_fixed_n(color_arr[i], 3, &color_obj);
        color[i].r = mp_obj_get_int(color_obj[0]);
        color[i].g = mp_obj_get_int(color_obj[1]);
        color[i].b = mp_obj_get_int(color_obj[2]);
    }

    /* Threshold image using reference color */
    imlib_threshold(image, &bimage, color, len, thresh);

    return py_image_from_struct(&bimage);
}

static mp_obj_t py_image_rainbow(mp_obj_t src_image_obj)
{
    image_t *src_image = NULL;

    /* get C image pointer */
    src_image = py_image_cobj(src_image_obj);

    /* sanity checks */
    PY_ASSERT_TRUE_MSG(src_image->bpp == 1,
            "This function is only supported on GRAYSCALE images");

    image_t dst_image = {
        .w=src_image->w,
        .h=src_image->h,
        .bpp=2,
        .pixels=xalloc(src_image->w*src_image->h*2)
    };

    imlib_rainbow(src_image, &dst_image);
    *src_image = dst_image;
    return src_image_obj;
}

static mp_obj_t py_image_compress(mp_obj_t image_obj, mp_obj_t quality)
{
    image_t *image = py_image_cobj(image_obj);

    image_t cimage = {
        .w=image->w,
        .h=image->h,
        .bpp = JPEG_INIT_BUF,
        .pixels = xalloc(JPEG_INIT_BUF)
    };

    jpeg_compress(image, &cimage, mp_obj_get_int(quality));

    return py_image_from_struct(&cimage);
}

static mp_obj_t py_image_find_blobs(mp_obj_t image_obj)
{
     // Get image pointer
    image_t *image = py_image_cobj(image_obj);

    // Run blob detector
    array_t *blobs = imlib_count_blobs(image);

    // Add detected blobs to a new Python list
    mp_obj_t objects_list = mp_obj_new_list(0, NULL);
    if (array_length(blobs)) {
        for (int j=0; j<array_length(blobs); j++) {
            blob_t *r = array_at(blobs, j);
            mp_obj_t blob[6] = {
                mp_obj_new_int(r->x), mp_obj_new_int(r->y), mp_obj_new_int(r->w),
                mp_obj_new_int(r->h), mp_obj_new_int(r->c), mp_obj_new_int(r->id)
            };
            mp_obj_list_append(objects_list, mp_obj_new_tuple(6, blob));
        }
    }
    array_free(blobs);
    return objects_list;
}

static mp_obj_t py_image_find_features(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    rectangle_t roi;
    image_t *image = NULL;
    cascade_t *cascade = NULL;

    // Sanity checks
    PY_ASSERT_TRUE_MSG(sensor.pixformat == PIXFORMAT_GRAYSCALE,
            "This function is only supported on GRAYSCALE images");

    PY_ASSERT_TRUE_MSG(sensor.framesize <= OMV_MAX_INT_FRAME,
            "This function is only supported on "OMV_MAX_INT_FRAME_STR" and smaller frames");

    // Read positional arguments
    image = py_image_cobj(args[0]);
    cascade = py_cascade_cobj(args[1]);

    // Read keyword arguments
    py_helper_lookup_rectangle(kw_args, image, &roi);
    cascade->threshold = py_helper_lookup_float(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_threshold), 0.5f);
    cascade->scale_factor = py_helper_lookup_float(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_scale), 1.5f);

    // Make sure ROI is not negative
    PY_ASSERT_FALSE_MSG((roi.x < 0) || (roi.y < 0) || (roi.w < 0) || (roi.h < 0),
            "Region of interest is negative!");

    // Make sure ROI is bigger than feature size
    PY_ASSERT_TRUE_MSG((roi.w > cascade->window.w && roi.h > cascade->window.h),
            "Region of interest is smaller than detector window!");

    // Make sure ROI is smaller than image size
    PY_ASSERT_TRUE_MSG(((roi.x+roi.w) <= image->w && (roi.y+roi.h) <= image->h),
            "Region of interest is bigger than frame size!");

    // Detect objects
    array_t *objects_array = imlib_detect_objects(image, cascade, &roi);

    // Add detected objects to a new Python list
    mp_obj_t objects_list = mp_obj_new_list(0, NULL);
    for (int i=0; i<array_length(objects_array); i++) {
        rectangle_t *r = array_at(objects_array, i);
        mp_obj_t rec_obj[4] = {
            mp_obj_new_int(r->x),
            mp_obj_new_int(r->y),
            mp_obj_new_int(r->w),
            mp_obj_new_int(r->h),
        };
        mp_obj_list_append(objects_list, mp_obj_new_tuple(4, rec_obj));
    }

    // Free the objects array
    array_free(objects_array);

    return objects_list;
}

static mp_obj_t py_image_find_template(mp_obj_t image_obj, mp_obj_t template_obj, mp_obj_t threshold)
{
    struct rectangle r;
    struct image *image = NULL;
    struct image *template = NULL;
    mp_obj_t rec_obj[4];
    mp_obj_t obj=mp_const_none;

    /* sanity checks */
    PY_ASSERT_TRUE_MSG(sensor.pixformat == PIXFORMAT_GRAYSCALE,
            "This function is only supported on GRAYSCALE images");

    PY_ASSERT_TRUE_MSG(sensor.framesize <= OMV_MAX_INT_FRAME,
            "This function is only supported on "OMV_MAX_INT_FRAME_STR" and smaller frames");


    /* get C image pointer */
    image = py_image_cobj(image_obj);
    template= py_image_cobj(template_obj);

    float t = mp_obj_get_float(threshold);

    /* look for object */
    float corr = imlib_template_match(image, template, &r);
    //printf("t:%f coor:%f\n", (double)t, (double)corr);
    if (corr > t) {
        rec_obj[0] = mp_obj_new_int(r.x);
        rec_obj[1] = mp_obj_new_int(r.y);
        rec_obj[2] = mp_obj_new_int(r.w);
        rec_obj[3] = mp_obj_new_int(r.h);
        obj = mp_obj_new_tuple(4, rec_obj);
    }
    return obj;
}

static mp_obj_t py_image_find_keypoints(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    rectangle_t roi;
    image_t *image = py_image_cobj(args[0]);

    // Sanity checks
    PY_ASSERT_TRUE_MSG(sensor.pixformat == PIXFORMAT_GRAYSCALE,
            "This function is only supported on GRAYSCALE images");

    PY_ASSERT_TRUE_MSG(sensor.framesize <= OMV_MAX_INT_FRAME,
            "This function is only supported on "OMV_MAX_INT_FRAME_STR" and smaller frames");

    // Read keyword arguments
    py_helper_lookup_rectangle(kw_args, image, &roi);
    int threshold = py_helper_lookup_int(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_threshold), 32);
    bool normalized = py_helper_lookup_int(kw_args, MP_OBJ_NEW_QSTR(qstr_from_str("normalized")), false);

    // Run keypoint descriptor on ROI
    array_t *kpts= freak_find_keypoints(image, normalized, threshold, &roi);

    if (array_length(kpts)) {
        // Return keypoints MP object
        py_kp_obj_t *kp_obj = m_new_obj(py_kp_obj_t);
        kp_obj->base.type = &py_kp_type;
        kp_obj->kpts = kpts;
        kp_obj->threshold = threshold;
        kp_obj->normalized = normalized;
        return kp_obj;
    }
    return mp_const_none;
}

static mp_obj_t py_image_find_lbp(mp_obj_t image_obj, mp_obj_t roi_obj)
{
    image_t *image;
    py_lbp_obj_t *lbp_obj;

    image = py_image_cobj(image_obj);
    /* sanity checks */
    PY_ASSERT_TRUE_MSG(image->bpp == 1,
            "This function is only supported on GRAYSCALE images");

    mp_obj_t *array;
    mp_obj_get_array_fixed_n(roi_obj, 4, &array);

    rectangle_t roi = {
        mp_obj_get_int(array[0]),
        mp_obj_get_int(array[1]),
        mp_obj_get_int(array[2]),
        mp_obj_get_int(array[3]),
    };

    lbp_obj = m_new_obj(py_lbp_obj_t);
    lbp_obj->base.type = &py_lbp_type;
    lbp_obj->hist = imlib_lbp_cascade(image, &roi);
    return lbp_obj;
}

static mp_obj_t py_image_find_eyes(mp_obj_t image_obj, mp_obj_t roi_obj)
{
    image_t *image;

    image = py_image_cobj(image_obj);
    /* sanity checks */
    PY_ASSERT_TRUE_MSG(image->bpp == 1,
            "This function is only supported on GRAYSCALE images");

    mp_obj_t *array;
    mp_obj_get_array_fixed_n(roi_obj, 4, &array);

    rectangle_t roi = {
        mp_obj_get_int(array[0]),
        mp_obj_get_int(array[1]),
        mp_obj_get_int(array[2]),
        mp_obj_get_int(array[3]),
    };

    point_t iris;
    imlib_find_iris(image, &iris, &roi);

    mp_obj_t eyes_obj[2] = {
        mp_obj_new_int(iris.x),
        mp_obj_new_int(iris.y),
    };

    return mp_obj_new_tuple(2, eyes_obj);
}

/* Image file functions */
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_save_obj, 2, py_image_save);
/* Basic image functions */
STATIC MP_DEFINE_CONST_FUN_OBJ_1(py_image_width_obj, py_image_width);
STATIC MP_DEFINE_CONST_FUN_OBJ_1(py_image_height_obj, py_image_height);
STATIC MP_DEFINE_CONST_FUN_OBJ_1(py_image_format_obj, py_image_format);
STATIC MP_DEFINE_CONST_FUN_OBJ_1(py_image_size_obj, py_image_size);
STATIC MP_DEFINE_CONST_FUN_OBJ_3(py_image_get_pixel_obj, py_image_get_pixel);
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_image_set_pixel_obj, 4, 4, py_image_set_pixel);
/* Drawing functions */
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_draw_line_obj, 2, py_image_draw_line);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_draw_rectangle_obj, 2, py_image_draw_rectangle);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_draw_circle_obj, 4, py_image_draw_circle);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_draw_string_obj, 4, py_image_draw_string);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_draw_cross_obj, 3, py_image_draw_cross);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_draw_keypoints_obj, 2, py_image_draw_keypoints);
/* Binary functions */
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_binary_obj, 2, py_image_binary);
STATIC MP_DEFINE_CONST_FUN_OBJ_1(py_image_invert_obj, py_image_invert);
STATIC MP_DEFINE_CONST_FUN_OBJ_2(py_image_and_obj, py_image_and);
STATIC MP_DEFINE_CONST_FUN_OBJ_2(py_image_nand_obj, py_image_nand);
STATIC MP_DEFINE_CONST_FUN_OBJ_2(py_image_or_obj, py_image_or);
STATIC MP_DEFINE_CONST_FUN_OBJ_2(py_image_nor_obj, py_image_nor);
STATIC MP_DEFINE_CONST_FUN_OBJ_2(py_image_xor_obj, py_image_xor);
STATIC MP_DEFINE_CONST_FUN_OBJ_2(py_image_xnor_obj, py_image_xnor);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_pixels_obj, 1, py_image_pixels);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_centroid_obj, 1, py_image_centroid);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_orientation_radians_obj, 1, py_image_orientation_radians);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_orientation_degrees_obj, 1, py_image_orientation_degrees);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_erode_obj, 2, py_image_erode);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_dilate_obj, 2, py_image_dilate);
/* Background Subtraction (Frame Differencing) functions */
STATIC MP_DEFINE_CONST_FUN_OBJ_1(py_image_negate_obj, py_image_negate);
STATIC MP_DEFINE_CONST_FUN_OBJ_2(py_image_difference_obj, py_image_difference);

STATIC MP_DEFINE_CONST_FUN_OBJ_2(py_image_scale_obj, py_image_scale);
STATIC MP_DEFINE_CONST_FUN_OBJ_2(py_image_scaled_obj, py_image_scaled);
STATIC MP_DEFINE_CONST_FUN_OBJ_2(py_image_subimg_obj, py_image_subimg);
STATIC MP_DEFINE_CONST_FUN_OBJ_3(py_image_blit_obj, py_image_blit);
STATIC MP_DEFINE_CONST_FUN_OBJ_3(py_image_blend_obj, py_image_blend);
STATIC MP_DEFINE_CONST_FUN_OBJ_1(py_image_histeq_obj, py_image_histeq);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_median_obj, 1, py_image_median);
STATIC MP_DEFINE_CONST_FUN_OBJ_3(py_image_threshold_obj, py_image_threshold);
STATIC MP_DEFINE_CONST_FUN_OBJ_1(py_image_rainbow_obj, py_image_rainbow);
STATIC MP_DEFINE_CONST_FUN_OBJ_2(py_image_compress_obj, py_image_compress);

STATIC MP_DEFINE_CONST_FUN_OBJ_1(py_image_find_blobs_obj, py_image_find_blobs);
STATIC MP_DEFINE_CONST_FUN_OBJ_3(py_image_find_template_obj, py_image_find_template);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_find_features_obj, 2, py_image_find_features);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_find_keypoints_obj, 1, py_image_find_keypoints);
STATIC MP_DEFINE_CONST_FUN_OBJ_2(py_image_find_lbp_obj, py_image_find_lbp);
STATIC MP_DEFINE_CONST_FUN_OBJ_2(py_image_find_eyes_obj, py_image_find_eyes);

static const mp_map_elem_t locals_dict_table[] = {
    /* Image file functions */
    {MP_OBJ_NEW_QSTR(MP_QSTR_save),                (mp_obj_t)&py_image_save_obj},
    /* Basic image functions */
    {MP_OBJ_NEW_QSTR(MP_QSTR_width),               (mp_obj_t)&py_image_width_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_height),              (mp_obj_t)&py_image_height_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_format),              (mp_obj_t)&py_image_format_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_size),                (mp_obj_t)&py_image_size_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_get_pixel),           (mp_obj_t)&py_image_get_pixel_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_set_pixel),           (mp_obj_t)&py_image_set_pixel_obj},
    /* Drawing functions */
    {MP_OBJ_NEW_QSTR(MP_QSTR_draw_line),           (mp_obj_t)&py_image_draw_line_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_draw_rectangle),      (mp_obj_t)&py_image_draw_rectangle_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_draw_circle),         (mp_obj_t)&py_image_draw_circle_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_draw_string),         (mp_obj_t)&py_image_draw_string_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_draw_cross),          (mp_obj_t)&py_image_draw_cross_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_draw_keypoints),      (mp_obj_t)&py_image_draw_keypoints_obj},
    /* Binary functions */
    {MP_OBJ_NEW_QSTR(MP_QSTR_binary),              (mp_obj_t)&py_image_binary_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_invert),              (mp_obj_t)&py_image_invert_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_and),                 (mp_obj_t)&py_image_and_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_nand),                (mp_obj_t)&py_image_nand_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_or),                  (mp_obj_t)&py_image_or_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_nor),                 (mp_obj_t)&py_image_nor_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_xor),                 (mp_obj_t)&py_image_xor_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_xnor),                (mp_obj_t)&py_image_xnor_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_pixels),              (mp_obj_t)&py_image_pixels_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_centroid),            (mp_obj_t)&py_image_centroid_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_orientation_radians), (mp_obj_t)&py_image_orientation_radians_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_orientation_degrees), (mp_obj_t)&py_image_orientation_degrees_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_erode),               (mp_obj_t)&py_image_erode_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_dilate),              (mp_obj_t)&py_image_dilate_obj},
    /* Background Subtraction (Frame Differencing) functions */
    {MP_OBJ_NEW_QSTR(MP_QSTR_negate),              (mp_obj_t)&py_image_negate_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_difference),          (mp_obj_t)&py_image_difference_obj},

    {MP_OBJ_NEW_QSTR(MP_QSTR_scale),               (mp_obj_t)&py_image_scale_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_scaled),              (mp_obj_t)&py_image_scaled_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_subimg),              (mp_obj_t)&py_image_subimg_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_blit),                (mp_obj_t)&py_image_blit_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_blend),               (mp_obj_t)&py_image_blend_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_histeq),              (mp_obj_t)&py_image_histeq_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_median),              (mp_obj_t)&py_image_median_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_threshold),           (mp_obj_t)&py_image_threshold_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_rainbow),             (mp_obj_t)&py_image_rainbow_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_compress),            (mp_obj_t)&py_image_compress_obj},
    /* objects/feature detection */
    {MP_OBJ_NEW_QSTR(MP_QSTR_find_blobs),          (mp_obj_t)&py_image_find_blobs_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_find_template),       (mp_obj_t)&py_image_find_template_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_find_features),       (mp_obj_t)&py_image_find_features_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_find_keypoints),      (mp_obj_t)&py_image_find_keypoints_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_find_lbp),            (mp_obj_t)&py_image_find_lbp_obj},
    {MP_OBJ_NEW_QSTR(MP_QSTR_find_eyes),           (mp_obj_t)&py_image_find_eyes_obj},

    { NULL, NULL },
};

STATIC MP_DEFINE_CONST_DICT(locals_dict, locals_dict_table);

static const mp_obj_type_t py_image_type = {
    { &mp_type_type },
    .name  = MP_QSTR_image,
    .print = py_image_print,
    .buffer_p = { .get_buffer = py_image_get_buffer },
    .subscr = py_image_subscr,
    .locals_dict = (mp_obj_t)&locals_dict,
};

mp_obj_t py_image(int w, int h, int bpp, void *pixels)
{
    py_image_obj_t *o = m_new_obj(py_image_obj_t);
    o->base.type = &py_image_type;

    o->_cobj.w =w;
    o->_cobj.h =h;
    o->_cobj.bpp =bpp;
    o->_cobj.pixels =pixels;
    return o;
}

mp_obj_t py_image_from_struct(image_t *image)
{
    py_image_obj_t *o = m_new_obj(py_image_obj_t);
    o->base.type = &py_image_type;
    o->_cobj =*image;
    return o;
}

int py_image_descriptor_from_roi(image_t *image, const char *path, rectangle_t *roi)
{
    FIL fp;
    FRESULT res = FR_OK;

    array_t *kpts = freak_find_keypoints(image, false, 10, roi);
    printf("Save Descriptor: KPTS(%d)\n", array_length(kpts));
    printf("Save Descriptor: ROI(%d %d %d %d)\n", roi->x, roi->y, roi->w, roi->h);

    if (array_length(kpts)) {
        if ((res = f_open(&fp, path, FA_WRITE|FA_CREATE_ALWAYS)) == FR_OK) {
            res = freak_save_descriptor(&fp, kpts);
            f_close(&fp);
        }

        // File open/write error
        if (res != FR_OK) {
            nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, ffs_strerror(res)));
        }
    }
    return 0;
}


// The image module (it's different from the Image class!)
mp_obj_t py_image_load_image(mp_obj_t path_obj)
{
    mp_obj_t image_obj =NULL;
    struct image *image;
    const char *path = mp_obj_str_get_str(path_obj);
    image_obj = py_image(0, 0, 0, 0);

    /* get image pointer */
    image = py_image_cobj(image_obj);

    imlib_load_image(image, path);

    return image_obj;
}

mp_obj_t py_image_load_cascade(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    cascade_t cascade;
    const char *path = mp_obj_str_get_str(args[0]);

    // Load cascade from file or flash
    int res = imlib_load_cascade(&cascade, path);
    if (res != FR_OK) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, ffs_strerror(res)));
    }

    // Read the number of stages
    int stages = py_helper_lookup_int(kw_args, MP_OBJ_NEW_QSTR(qstr_from_str("stages")), cascade.n_stages);
    // Check the number of stages
    if (stages > 0 && stages < cascade.n_stages) {
        cascade.n_stages = stages;
    }

    // Return micropython cascade object
    py_cascade_obj_t *o = m_new_obj(py_cascade_obj_t);
    o->base.type = &py_cascade_type;
    o->_cobj = cascade;
    return o;
}

mp_obj_t py_image_load_descriptor(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    FIL fp;
    FRESULT res;

    mp_obj_t desc = mp_const_none;
    descriptor_t desc_type = mp_obj_get_int(args[0]);
    const char *path = mp_obj_str_get_str(args[1]);

    if ((res = f_open(&fp, path, FA_READ|FA_OPEN_EXISTING)) == FR_OK) {
        switch (desc_type) {
            case DESC_LBP: {
                py_lbp_obj_t *lbp = m_new_obj(py_lbp_obj_t);
                lbp->base.type = &py_lbp_type;

                res = imlib_lbp_desc_load(&fp, &lbp->hist);
                if (res == FR_OK) {
                    desc = lbp;
                }
                break;
            }

            case DESC_FREAK: {
                array_t *kpts = NULL;
                array_alloc(&kpts, xfree);

                res = freak_load_descriptor(&fp, kpts);
                if (res == FR_OK) {
                    // Return keypoints MP object
                    py_kp_obj_t *kp_obj = m_new_obj(py_kp_obj_t);
                    kp_obj->base.type = &py_kp_type;
                    kp_obj->kpts = kpts;
                    kp_obj->threshold = 10;
                    kp_obj->normalized = false;
                    desc = kp_obj;
                }
                break;
            }
        }

        f_close(&fp);
    }

    // File open or write error
    if (res != FR_OK) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, ffs_strerror(res)));
    }

    // If no file error and descriptor is still none, then it's not supported.
    if (desc == mp_const_none) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, "Descriptor type is not supported"));
    }
    return desc;
}

mp_obj_t py_image_save_descriptor(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    FIL fp;
    FRESULT res;

    descriptor_t desc_type = mp_obj_get_int(args[0]);
    const char *path = mp_obj_str_get_str(args[1]);

    if ((res = f_open(&fp, path, FA_WRITE|FA_CREATE_ALWAYS)) == FR_OK) {
        switch (desc_type) {
            case DESC_LBP: {
                py_lbp_obj_t *lbp = ((py_lbp_obj_t*)args[2]);
                res = imlib_lbp_desc_save(&fp, lbp->hist);
                break;
            }

            case DESC_FREAK: {
                py_kp_obj_t *kpts = ((py_kp_obj_t*)args[2]);
                res = freak_save_descriptor(&fp, kpts->kpts);
                break;
            }
        }
        // ignore unsupported descriptors when saving

        f_close(&fp);
    }

    // File open or read error
    if (res != FR_OK) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, ffs_strerror(res)));
    }
    return mp_const_true;
}

static mp_obj_t py_image_match_descriptor(uint n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    mp_obj_t match_obj = mp_const_none;
    descriptor_t desc_type = mp_obj_get_int(args[0]);

    switch (desc_type) {
        case DESC_LBP: {
            py_lbp_obj_t *lbp1 = ((py_lbp_obj_t*)args[1]);
            py_lbp_obj_t *lbp2 = ((py_lbp_obj_t*)args[2]);

            // Sanity checks
            PY_ASSERT_TYPE(lbp1, &py_lbp_type);
            PY_ASSERT_TYPE(lbp2, &py_lbp_type);

            match_obj = mp_obj_new_int(imlib_lbp_desc_distance(lbp1->hist, lbp2->hist));
            break;
        }

        case DESC_FREAK: {
            py_kp_obj_t *kpts1 = ((py_kp_obj_t*)args[1]);
            py_kp_obj_t *kpts2 = ((py_kp_obj_t*)args[2]);
            int threshold = py_helper_lookup_int(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_threshold), 60);

            // Sanity checks
            PY_ASSERT_TYPE(kpts1, &py_kp_type);
            PY_ASSERT_TYPE(kpts2, &py_kp_type);
            PY_ASSERT_TRUE_MSG((threshold >=0 && threshold <= 100), "Expected threshold between 0 and 100");

            int match=0, cx=0, cy=0;
            // Match the two keypoint sets
            // Returns the number of matches
            match = freak_match_keypoints(kpts1->kpts, kpts2->kpts, threshold);

            if (match) {
                for (int i=0; i<array_length(kpts1->kpts); i++) {
                    kp_t *kp = array_at(kpts1->kpts, i);
                    if (kp->match != NULL) {
                        cx += kp->match->x;
                        cy += kp->match->y;
                    }
                }
            }

            mp_obj_t rec_obj[3] = {
                mp_obj_new_int(cx/match),
                mp_obj_new_int(cy/match),
                mp_obj_new_int(match*100/IM_MAX(array_length(kpts1->kpts), 1))
            };
            match_obj = mp_obj_new_tuple(3, rec_obj);
        }
    }

    return match_obj;
}


STATIC MP_DEFINE_CONST_FUN_OBJ_1(py_image_load_image_obj, py_image_load_image);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_load_cascade_obj, 1, py_image_load_cascade);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_load_descriptor_obj, 2, py_image_load_descriptor);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_save_descriptor_obj, 3, py_image_save_descriptor);
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(py_image_match_descriptor_obj, 3, py_image_match_descriptor);

static const mp_map_elem_t globals_dict_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__),            MP_OBJ_NEW_QSTR(MP_QSTR_image) },
    // Descriptor types
    { MP_OBJ_NEW_QSTR(MP_QSTR_LBP),                 MP_OBJ_NEW_SMALL_INT(DESC_LBP)},
    { MP_OBJ_NEW_QSTR(MP_QSTR_FREAK),               MP_OBJ_NEW_SMALL_INT(DESC_FREAK)},

    // Image module functions
    { MP_OBJ_NEW_QSTR(MP_QSTR_Image),               (mp_obj_t)&py_image_load_image_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_HaarCascade),         (mp_obj_t)&py_image_load_cascade_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_load_descriptor),     (mp_obj_t)&py_image_load_descriptor_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_save_descriptor),     (mp_obj_t)&py_image_save_descriptor_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_match_descriptor),    (mp_obj_t)&py_image_match_descriptor_obj },
};
STATIC MP_DEFINE_CONST_DICT(globals_dict, globals_dict_table);

const mp_obj_module_t image_module = {
    .base = { &mp_type_module },
    .name = MP_QSTR_image,
    .globals = (mp_obj_t)&globals_dict,
};
