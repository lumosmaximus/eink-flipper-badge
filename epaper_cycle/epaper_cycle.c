#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
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

#define CYCLE_TOTAL_MS 60000   // 60 seconds per image, start to start

// ================= App state =================
typedef struct {
    char name[MAX_NAME_LEN];
} ImageEntry;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    ImageEntry images[MAX_IMAGES];
    uint32_t count;
    char cur_name[MAX_NAME_LEN];
    volatile bool running;   // set false to exit
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

// Stream image with 180-degree rotation (panel mounted upside-down on stand).
static bool epd_show_image_file(const char* path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
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

static void display_file(App* app, const char* name) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", IMG_DIR, name);
    strncpy(app->cur_name, name, MAX_NAME_LEN - 1);
    app->cur_name[MAX_NAME_LEN - 1] = '\0';
    view_port_update(app->view_port);

    pins_init();
    if(epd_init()) {
        if(epd_show_image_file(path)) {
            epd_turn_on_display();
        }
    }
    pins_deinit();
}

// ================= Scanning =================
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

// ================= UI (Flipper screen) =================
static void draw_callback(Canvas* canvas, void* ctx) {
    App* app = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 6, AlignCenter, AlignTop, "Auto-Cycle");
    canvas_set_font(canvas, FontSecondary);
    if(app->count == 0) {
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignTop, "No .bin images found");
        canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignTop, "in apps/epaper");
    } else {
        char nm[24];
        strncpy(nm, app->cur_name, sizeof(nm) - 1);
        nm[sizeof(nm) - 1] = '\0';
        canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignTop, "Showing:");
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignTop, nm);
    }
    canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignTop, "Back = exit");
}

static void input_callback(InputEvent* event, void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->input_queue, event, 0);
}

// interruptible wait: returns false if Back was pressed (exit requested)
static bool wait_ms_or_back(App* app, uint32_t ms) {
    uint32_t waited = 0;
    InputEvent event;
    while(waited < ms) {
        if(furi_message_queue_get(app->input_queue, &event, 50) == FuriStatusOk) {
            if(event.type == InputTypeShort && event.key == InputKeyBack) {
                app->running = false;
                return false;
            }
        }
        waited += 50;
    }
    return true;
}

int32_t epaper_cycle_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->running = true;

    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    scan_images(app);
    view_port_update(app->view_port);

    if(app->count == 0) {
        // Nothing to show: wait for Back, then exit.
        InputEvent event;
        while(app->running) {
            if(furi_message_queue_get(app->input_queue, &event, 100) == FuriStatusOk) {
                if(event.type == InputTypeShort && event.key == InputKeyBack) break;
            }
        }
    } else {
        // Cycle forever, 60s per image, until Back is pressed.
        uint32_t i = 0;
        while(app->running) {
            uint32_t start = furi_get_tick();
            display_file(app, app->images[i].name);   // ~30s incl. refresh
            // wait out the remainder of the 60s window (Back-interruptible)
            uint32_t elapsed = furi_get_tick() - start;
            if(elapsed < CYCLE_TOTAL_MS) {
                if(!wait_ms_or_back(app, CYCLE_TOTAL_MS - elapsed)) break;
            }
            i++;
            if(i >= app->count) i = 0;
        }
    }

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->input_queue);
    free(app);
    return 0;
}