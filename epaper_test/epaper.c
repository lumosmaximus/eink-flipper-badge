#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/view.h>
#include <storage/storage.h>

// ---------- Pins ----------
#define PIN_PWR   &gpio_ext_pc1
#define PIN_DIN   &gpio_ext_pa7
#define PIN_SCLK  &gpio_ext_pb3
#define PIN_CS    &gpio_ext_pa4
#define PIN_DC    &gpio_ext_pb2
#define PIN_RST   &gpio_ext_pc3
#define PIN_BUSY  &gpio_ext_pc0

#define EPD_WIDTH   400
#define EPD_HEIGHT  600
#define EPD_BUFSIZE (EPD_WIDTH * EPD_HEIGHT / 2)  // 120000
#define CHUNK 4096

#define IMG_DIR       "/ext/apps/epaper"
#define MAX_IMAGES    32
#define MAX_NAME_LEN  64

#define IDLE_TIMEOUT_MS  120000
#define TICK_INTERVAL_MS 500      // twice a second (drives the dot animation)

#define SLIDESHOW_PAUSE_MS 4000

#define VIEW_MENU     0
#define VIEW_STATUS   1

#define MENU_SLIDESHOW 1000

// ================= App state =================
typedef struct {
    char name[MAX_NAME_LEN];
} ImageEntry;

typedef struct {
    ViewDispatcher* vd;
    Submenu* submenu;
    View* status_view;
    Gui* gui;
    ImageEntry images[MAX_IMAGES];
    uint32_t count;
    uint32_t idle_ms;
    bool busy;

    // status screen state
    char cur_name[MAX_NAME_LEN];
    const char* cur_phase;   // "Drawing" / "Done"
    uint8_t anim;            // animation counter for the dots

    bool slideshow;
    bool stop_slideshow;
} App;

// ================= TRANSFER CODE (driver) =================
static void spi_write_byte(uint8_t value) {
    for(int i = 0; i < 8; i++) {
        furi_hal_gpio_write(PIN_DIN, (value & 0x80) != 0);
        furi_hal_gpio_write(PIN_SCLK, true);
        furi_hal_gpio_write(PIN_SCLK, false);
        value <<= 1;
    }
}
static void send_command(uint8_t cmd) {
    furi_hal_gpio_write(PIN_DC, false);
    furi_hal_gpio_write(PIN_CS, false);
    spi_write_byte(cmd);
    furi_hal_gpio_write(PIN_CS, true);
}
static void send_data(uint8_t data) {
    furi_hal_gpio_write(PIN_DC, true);
    furi_hal_gpio_write(PIN_CS, false);
    spi_write_byte(data);
    furi_hal_gpio_write(PIN_CS, true);
}
static void epd_reset(void) {
    furi_hal_gpio_write(PIN_RST, true);  furi_delay_ms(20);
    furi_hal_gpio_write(PIN_RST, false); furi_delay_ms(2);
    furi_hal_gpio_write(PIN_RST, true);  furi_delay_ms(20);
}
static bool wait_busy(uint32_t timeout_ms) {
    uint32_t waited = 0;
    while(!furi_hal_gpio_read(PIN_BUSY)) {
        furi_delay_ms(5); waited += 5;
        if(waited >= timeout_ms) return false;
    }
    furi_delay_ms(200);
    return true;
}
static bool epd_init(void) {
    epd_reset();
    if(!wait_busy(5000)) return false;
    furi_delay_ms(30);
    send_command(0xAA);
    send_data(0x49); send_data(0x55); send_data(0x20);
    send_data(0x08); send_data(0x09); send_data(0x18);
    send_command(0x01); send_data(0x3F);
    send_command(0x00); send_data(0x5F); send_data(0x69);
    send_command(0x05); send_data(0x40); send_data(0x1F); send_data(0x1F); send_data(0x2C);
    send_command(0x08); send_data(0x6F); send_data(0x1F); send_data(0x1F); send_data(0x22);
    send_command(0x06); send_data(0x6F); send_data(0x1F); send_data(0x17); send_data(0x17);
    send_command(0x03); send_data(0x00); send_data(0x54); send_data(0x00); send_data(0x44);
    send_command(0x60); send_data(0x02); send_data(0x00);
    send_command(0x30); send_data(0x08);
    send_command(0x50); send_data(0x3F);
    send_command(0x61); send_data(0x01); send_data(0x90); send_data(0x02); send_data(0x58);
    send_command(0xE3); send_data(0x2F);
    send_command(0x84); send_data(0x01);
    if(!wait_busy(5000)) return false;
    return true;
}
static bool epd_turn_on_display(void) {
    send_command(0x04);
    if(!wait_busy(10000)) return false;
    furi_delay_ms(200);
    send_command(0x06); send_data(0x6F); send_data(0x1F); send_data(0x17); send_data(0x27);
    furi_delay_ms(200);
    send_command(0x12); send_data(0x00);
    if(!wait_busy(40000)) return false;
    send_command(0x02); send_data(0x00);
    if(!wait_busy(10000)) return false;
    furi_delay_ms(200);
    return true;
}
static bool epd_show_image_file(const char* path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        // Read the whole image into RAM so we can send it reversed
        uint8_t* img = malloc(EPD_BUFSIZE);
        uint32_t total = 0;
        uint16_t n;
        do {
            n = storage_file_read(file, img + total, CHUNK);
            total += n;
        } while(n == CHUNK && total < EPD_BUFSIZE);
        storage_file_close(file);

        if(total == EPD_BUFSIZE) {
            send_command(0x10);
            furi_hal_gpio_write(PIN_DC, true);
            furi_hal_gpio_write(PIN_CS, false);
            // 180 rotation: bytes in reverse order, nibbles swapped in each
            for(int32_t i = EPD_BUFSIZE - 1; i >= 0; i--) {
                uint8_t b = img[i];
                spi_write_byte((b << 4) | (b >> 4));
            }
            furi_hal_gpio_write(PIN_CS, true);
            ok = true;
        }
        free(img);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}
static void pins_init(void) {
    furi_hal_gpio_init_simple(PIN_PWR, GpioModeOutputPushPull);
    furi_hal_gpio_write(PIN_PWR, true); furi_delay_ms(10);
    furi_hal_gpio_init_simple(PIN_DIN,  GpioModeOutputPushPull);
    furi_hal_gpio_init_simple(PIN_SCLK, GpioModeOutputPushPull);
    furi_hal_gpio_init_simple(PIN_CS,   GpioModeOutputPushPull);
    furi_hal_gpio_init_simple(PIN_DC,   GpioModeOutputPushPull);
    furi_hal_gpio_init_simple(PIN_RST,  GpioModeOutputPushPull);
    furi_hal_gpio_init(PIN_BUSY, GpioModeInput, GpioPullUp, GpioSpeedLow);
    furi_hal_gpio_write(PIN_CS, true);
    furi_hal_gpio_write(PIN_SCLK, false);
    furi_hal_gpio_write(PIN_RST, true);
}
static void pins_deinit(void) {
    furi_hal_gpio_init_simple(PIN_PWR,  GpioModeAnalog);
    furi_hal_gpio_init_simple(PIN_DIN,  GpioModeAnalog);
    furi_hal_gpio_init_simple(PIN_SCLK, GpioModeAnalog);
    furi_hal_gpio_init_simple(PIN_CS,   GpioModeAnalog);
    furi_hal_gpio_init_simple(PIN_DC,   GpioModeAnalog);
    furi_hal_gpio_init_simple(PIN_RST,  GpioModeAnalog);
    furi_hal_gpio_init_simple(PIN_BUSY, GpioModeAnalog);
}

// Run one full display. Status screen is already showing "Drawing".
static void display_file(App* app, const char* name) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", IMG_DIR, name);
    strncpy(app->cur_name, name, MAX_NAME_LEN - 1);
    app->cur_name[MAX_NAME_LEN - 1] = '\0';
    app->cur_phase = "Drawing";
    view_dispatcher_switch_to_view(app->vd, VIEW_STATUS);

    pins_init();
    if(epd_init()) {
        if(epd_show_image_file(path)) {
            epd_turn_on_display();
        }
    }
    pins_deinit();
    app->cur_phase = "Done";
}

// ================= Status view =================
static void status_draw_callback(Canvas* canvas, void* model) {
    App* app = *(App**)model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 4, AlignCenter, AlignTop, "E-Paper Badge");

    canvas_set_font(canvas, FontSecondary);
    char nm[24];
    strncpy(nm, app->cur_name, sizeof(nm) - 1);
    nm[sizeof(nm) - 1] = '\0';
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignTop, nm);

    // Phase line with animated dots while drawing
    char line[24];
    if(strcmp(app->cur_phase, "Drawing") == 0) {
        uint8_t dots = app->anim % 4;      // 0..3 dots
        char d[5] = "";
        for(uint8_t i = 0; i < dots; i++) d[i] = '.';
        d[dots] = '\0';
        snprintf(line, sizeof(line), "Drawing%s", d);
    } else {
        snprintf(line, sizeof(line), "Done");
    }
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignTop, line);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignTop, "~30s  6-color refresh");
}

static bool status_input_callback(InputEvent* event, void* context) {
    App* app = context;
    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        if(app->slideshow) { app->stop_slideshow = true; return true; }
    }
    return false;
}

// ================= Menu / scanning =================
static bool has_bin_ext(const char* name) {
    size_t len = strlen(name);
    if(len < 4) return false;
    const char* e = name + len - 4;
    return (e[0]=='.') && (e[1]=='b'||e[1]=='B') && (e[2]=='i'||e[2]=='I') && (e[3]=='n'||e[3]=='N');
}
static void scan_images(App* app) {
    app->count = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage_dir_exists(storage, IMG_DIR)) storage_simply_mkdir(storage, IMG_DIR);
    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, IMG_DIR)) {
        FileInfo info;
        char name[MAX_NAME_LEN];
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;
            if(!has_bin_ext(name)) continue;
            if(app->count >= MAX_IMAGES) break;
            strncpy(app->images[app->count].name, name, MAX_NAME_LEN - 1);
            app->images[app->count].name[MAX_NAME_LEN - 1] = '\0';
            app->count++;
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
}

static void run_slideshow(App* app) {
    app->slideshow = true;
    app->stop_slideshow = false;
    for(uint32_t i = 0; i < app->count && !app->stop_slideshow; i++) {
        display_file(app, app->images[i].name);
        uint32_t waited = 0;
        while(waited < SLIDESHOW_PAUSE_MS && !app->stop_slideshow) {
            furi_delay_ms(100); waited += 100;
        }
    }
    app->slideshow = false;
    view_dispatcher_switch_to_view(app->vd, VIEW_MENU);
}

static void submenu_callback(void* context, uint32_t index) {
    App* app = context;
    app->idle_ms = 0;
    app->busy = true;

    if(index == MENU_SLIDESHOW) {
        run_slideshow(app);
    } else if(index < app->count) {
        display_file(app, app->images[index].name);
        furi_delay_ms(1200);   // let "Done" show briefly
        view_dispatcher_switch_to_view(app->vd, VIEW_MENU);
    }

    app->busy = false;
    app->idle_ms = 0;
}

static void tick_callback(void* context) {
    App* app = context;
    // advance the dot animation and redraw the status screen
    app->anim++;
    with_view_model(app->status_view, App** m, { *m = app; }, true);

    if(app->busy) { app->idle_ms = 0; return; }
    app->idle_ms += TICK_INTERVAL_MS;
    if(app->idle_ms >= IDLE_TIMEOUT_MS) view_dispatcher_stop(app->vd);
}

static uint32_t exit_to_menu(void* context) {
    UNUSED(context);
    return VIEW_MENU;
}
static uint32_t exit_app(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

int32_t epaper_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));

    app->gui = furi_record_open(RECORD_GUI);
    app->vd = view_dispatcher_alloc();
    app->submenu = submenu_alloc();
    app->cur_phase = "";

    scan_images(app);

    if(app->count > 0) {
        submenu_add_item(app->submenu, "> Slideshow", MENU_SLIDESHOW, submenu_callback, app);
    }
    if(app->count == 0) {
        submenu_add_item(app->submenu, "No .bin in apps/epaper", 0, NULL, app);
    } else {
        for(uint32_t i = 0; i < app->count; i++) {
            submenu_add_item(app->submenu, app->images[i].name, i, submenu_callback, app);
        }
    }

    app->status_view = view_alloc();
    view_allocate_model(app->status_view, ViewModelTypeLocking, sizeof(App*));
    with_view_model(app->status_view, App** m, { *m = app; }, true);
    view_set_draw_callback(app->status_view, status_draw_callback);
    view_set_input_callback(app->status_view, status_input_callback);
    view_set_context(app->status_view, app);
    view_set_previous_callback(app->status_view, exit_to_menu);

    view_set_previous_callback(submenu_get_view(app->submenu), exit_app);
    view_dispatcher_add_view(app->vd, VIEW_MENU, submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->vd, VIEW_STATUS, app->status_view);

    view_dispatcher_set_event_callback_context(app->vd, app);
    view_dispatcher_set_tick_event_callback(app->vd, tick_callback, TICK_INTERVAL_MS);

    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->vd, VIEW_MENU);
    view_dispatcher_run(app->vd);

    view_dispatcher_remove_view(app->vd, VIEW_MENU);
    view_dispatcher_remove_view(app->vd, VIEW_STATUS);
    submenu_free(app->submenu);
    view_free(app->status_view);
    view_dispatcher_free(app->vd);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}