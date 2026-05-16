#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// ==================== АППАРАТНЫЕ ПИНЫ CYD 2.8" ====================
#define SCREEN_WIDTH   320  
#define SCREEN_HEIGHT  240
#define TOUCH_CS       33
#define TOUCH_CLK      25
#define TOUCH_MOSI     32
#define TOUCH_MISO     39
#define BACKLIGHT_PIN  21
#define BUZZER_PIN     22

// ==================== КАЛИБРОВКА ТАЧА БЕЗ ЗДВИГОВ ====================
#define TS_MIN_X  250
#define TS_MAX_X  3850
#define TS_MIN_Y  200
#define TS_MAX_Y  3750

// ==================== PALETTE: MATERIAL YOU DARK ====================
#define AND_BG          0x1082   // Чёрный фон игры
#define AND_SURFACE     0x18E3   // Панель сайдбара
#define AND_SURFACE3    0x2945   // Пассивные кнопки
#define AND_ACCENT      0x03DF   // Активный инструмент (Google Blue)
#define AND_RED         0xF800   // Шар 3: Мясо (Красный)
#define AND_PINK        0xFDB8   // Шар 2: Плоть (Розовый)
#define AND_TEXT        0xFFFF   // Шар 1: Кожа (Белый)
#define AND_TEXT3       0x7BEF   // Скелетные линии (Серый)
#define COLOR_ROD       0xBDF7   // Стальная палка
#define COLOR_HAMMER    0x4208   // Молот
#define COLOR_HEART     0x8000   
#define COLOR_LUNG      0xFC10   
#define COLOR_INTESTINE 0xD34D   

#define SIDEBAR_W      50
#define PLAY_X_MIN     (SIDEBAR_W + 4)
#define PLAY_X_MAX     (SCREEN_WIDTH - 4)
#define PLAY_Y_MIN     4
#define PLAY_Y_MAX     (SCREEN_HEIGHT - 4)

// Плавная физика из People Playground
#define GRAVITY        0.25f    
#define AIR_RESISTANCE 0.96f    

enum ToolMode { TOOL_DRAG, TOOL_SPAWN, TOOL_ROD, TOOL_HAMMER, TOOL_KNIFE, TOOL_DELETE };
ToolMode currentTool = TOOL_DRAG;
bool inMenu = true; 

struct Blood { float x, y, vx, vy; bool active = false; };
const int MAX_BLOOD = 40;
Blood bloodPool[MAX_BLOOD];

enum OrganType { ORGAN_HEART, ORGAN_LUNG, ORGAN_INTESTINE };
struct Organ { float x, y, vx, vy; OrganType type; bool active = false; };
const int MAX_ORGANS = 12;
Organ organPool[MAX_ORGANS];

// Структура Solid Frame манекена
struct BonePart {
    float x, y;       // Координаты центра куска
    float vx, vy;     // Скорость куска
    int w, h;         // Размеры
    float offX, offY; // Жёсткое анатомическое смещение относительно Торса
    int skinHP;       // Разрушаемость (100 -> 0)
    bool boneBroken;  // Слом кости (только молотом)
    bool isAmputated; // Отрезание конечности (только ножом)
};

const int NODE_COUNT = 6; // 0-Голова, 1-Торс, 2-Л_Рука, 3-П_Рука, 4-Л_Нога, 5-П_Нога
BonePart body[NODE_COUNT];
bool dollExists = false;
int grabbedNodeIndex = -1;

float propX = 100, propY = 50;
float propTargetX = 100, propTargetY = 50;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite buffer = TFT_eSprite(&tft); 
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS);

void beep(uint16_t f=800, uint16_t d=25) { tone(BUZZER_PIN, f, d); }

bool getAlignedTouch(int &screenX, int &screenY) {
    if (touch.touched()) {
        TS_Point p = touch.getPoint();
        if (p.z < 100) return false;
        screenX = map(p.x, TS_MIN_X, TS_MAX_X, 0, SCREEN_WIDTH);
        screenY = map(p.y, TS_MIN_Y, TS_MAX_Y, 0, SCREEN_HEIGHT);
        if (screenX < 0) screenX = 0; if (screenX > SCREEN_WIDTH) screenX = SCREEN_WIDTH;
        if (screenY < 0) screenY = 0; if (screenY > SCREEN_HEIGHT) screenY = SCREEN_HEIGHT;
        return true;
    }
    return false;
}

void setNode(int index, float x, float y, int w, int h, float offX, float offY) {
    body[index].x = x + offX;  body[index].y = y + offY;
    body[index].vx = 0;        body[index].vy = 0;
    body[index].w = w;         body[index].h = h;
    body[index].offX = offX;   body[index].offY = offY;
    body[index].skinHP = 100;
    body[index].boneBroken = false;
    body[index].isAmputated = false;
}

void spawnDoll(float sx, float sy) {
    if (sx < PLAY_X_MIN + 30) sx = PLAY_X_MIN + 30;
    if (sx > PLAY_X_MAX - 30) sx = PLAY_X_MAX - 30;
    if (sy > PLAY_Y_MAX - 65) sy = PLAY_Y_MAX - 65; 
    
    // ЖЁСТКАЯ АНАТОМИЧЕСКАЯ СБОРКА
    setNode(1, sx, sy, 20, 34,  0,   0);   // Торс (Центр, индекс 1)
    setNode(0, sx, sy, 24, 24,  0, -28);   // Крупная Голова
    setNode(2, sx, sy,  8, 22, -15,  0);   // Левая рука
    setNode(3, sx, sy,  8, 22,  15,  0);   // Правая рука
    setNode(4, sx, sy, 10, 24, -6,  28);   // Левая нога
    setNode(5, sx, sy, 10, 24,  6,  28);   // Правая нога
    
    grabbedNodeIndex = -1;
    dollExists = true;
    for (int i = 0; i < MAX_ORGANS; i++) organPool[i].active = false;
}

void emitBlood(float x, float y) {
    int count = 0;
    for (int i = 0; i < MAX_BLOOD; i++) {
        if (!bloodPool[i].active) {
            bloodPool[i].x = x; bloodPool[i].y = y;
            bloodPool[i].vx = random(-40, 41) / 10.0f;
            bloodPool[i].vy = random(-45, -5) / 10.0f;
            bloodPool[i].active = true;
            count++; if (count > 4) break;
        }
    }
}

void triggerSpecificOrganDrop(float x, float y) {
    for (int i = 0; i < MAX_ORGANS; i++) {
        if (!organPool[i].active) {
            organPool[i].x = x + random(-3, 4); organPool[i].y = y + random(-3, 4);
            organPool[i].vx = random(-15, 16) / 10.0f; organPool[i].vy = random(-20, -5) / 10.0f;
            int r = random(0, 3);
            if (r == 0)      organPool[i].type = ORGAN_HEART;
            else if (r == 1) organPool[i].type = ORGAN_LUNG;
            else             organPool[i].type = ORGAN_INTESTINE;
            organPool[i].active = true;
            break;
        }
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(BACKLIGHT_PIN, OUTPUT); digitalWrite(BACKLIGHT_PIN, HIGH); 
    touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    touch.begin(touchSPI); touch.setRotation(1); 
    tft.init(); tft.setRotation(1); 
    buffer.setColorDepth(8); buffer.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
    spawnDoll(180, 80);
}

void loop() {
    int tx = -1, ty = -1;
    bool touched = getAlignedTouch(tx, ty);
    
    static bool wasTouched = false;
    bool clickTriggered = touched && !wasTouched;
    wasTouched = touched;

    if (!touched) grabbedNodeIndex = -1;

    // Релизный лаунчер AndOS Material You
    if (inMenu) {
        buffer.fillSprite(AND_BG);
        buffer.fillRoundRect(30, 30, SCREEN_WIDTH - 60, SCREEN_HEIGHT - 60, 12, AND_SURFACE);
        buffer.drawRoundRect(30, 30, SCREEN_WIDTH - 60, SCREEN_HEIGHT - 60, 12, AND_TEXT3);
        buffer.setTextColor(AND_TEXT); buffer.setTextSize(2); buffer.setTextDatum(MC_DATUM);
        buffer.drawString("CAKE PLAYGROUND", SCREEN_WIDTH/2, SCREEN_HEIGHT/3);
        buffer.setTextColor(AND_TEXT3); buffer.setTextSize(1);
        buffer.drawString("v1.5 Fixed Full Build", SCREEN_WIDTH/2, SCREEN_HEIGHT/3 + 22);

        bool hover = (touched && tx > 70 && tx < SCREEN_WIDTH - 70 && ty > 130 && ty < 175);
        buffer.fillRoundRect(70, 130, SCREEN_WIDTH - 140, 45, 8, hover ? AND_ACCENT : AND_SURFACE3);
        buffer.drawRoundRect(70, 130, SCREEN_WIDTH - 140, 45, 8, AND_TEXT3);
        buffer.setTextColor(AND_TEXT); buffer.drawString("START GAME", SCREEN_WIDTH/2, 152);

        if (clickTriggered && hover) { beep(880, 60); inMenu = false; }
        buffer.pushSprite(0, 0); delay(15); return; 
    }

    buffer.fillSprite(AND_BG);
    buffer.drawRect(PLAY_X_MIN, PLAY_Y_MIN, (PLAY_X_MAX - PLAY_X_MIN), (PLAY_Y_MAX - PLAY_Y_MIN), AND_TEXT3);

    // Сайдбар меню
    if (touched && tx < SIDEBAR_W) {
        if (clickTriggered) {
            if (ty >= 5 && ty <= 35)   currentTool = TOOL_SPAWN;
            if (ty >= 42 && ty <= 72)  currentTool = TOOL_DRAG;
            if (ty >= 79 && ty <= 109) currentTool = TOOL_ROD;
            if (ty >= 116 && ty <= 146) currentTool = TOOL_HAMMER;
            if (ty >= 153 && ty <= 183) currentTool = TOOL_KNIFE;
            if (ty >= 190 && ty <= 220) { inMenu = true; return; } 
        }
        touched = false; 
    }

    if (touched && tx >= PLAY_X_MIN) {
        if (currentTool == TOOL_SPAWN && clickTriggered) { spawnDoll(tx, ty); currentTool = TOOL_DRAG; touched = false; }
        else if (currentTool == TOOL_ROD || currentTool == TOOL_HAMMER || currentTool == TOOL_KNIFE) { propTargetX = tx; propTargetY = ty; }
    }

    propX += (propTargetX - propX) * 0.45f; propY += (propTargetY - propY) * 0.45f;

    // РОБОТА РУШІЯ SOLID FRAME З ПОВНОЮ КОЛІЗІЄЮ КОЖНОЇ КІНЦІВКИ
    if (dollExists) {
        // Інтерактивний хват DRAG
        if (touched && currentTool == TOOL_DRAG) {
            if (grabbedNodeIndex == -1) {
                float minDist = 999.0f;
                for (int i = 0; i < NODE_COUNT; i++) {
                    float dx = tx - body[i].x; float dy = ty - body[i].y;
                    float dist = sqrt(dx * dx + dy * dy);
                    if (dist < 30 && dist < minDist) { minDist = dist; grabbedNodeIndex = i; }
                }
            }
            if (grabbedNodeIndex != -1) {
                int i = grabbedNodeIndex;
                if (i == 1) {
                    body[1].x += (tx - body[1].x) * 0.45f; body[1].y += (ty - body[1].y) * 0.45f;
                } else {
                    if (body[i].isAmputated) { 
                        body[i].x += (tx - body[i].x) * 0.45f; body[i].y += (ty - body[i].y) * 0.45f;
                    } else {
                        // ФІКС ІНДЕКСУ: Підтягуємо Торс через індекс [1]
                        body[1].x += (tx - body[i].offX - body[1].x) * 0.4f;
                        body[1].y += (ty - body[i].offY - body[1].y) * 0.4f;
                    }
                }
                if (random(0, 100) > 85) emitBlood(body[i].x, body[i].y);
            }
        }

        // Застосування швидкостей та гравітації до головного Торса за індексом [1]
        body[1].vy += GRAVITY;
        body[1].x += body[1].vx; body[1].y += body[1].vy;
        body[1].vx *= AIR_RESISTANCE;

        // Колізія Торса з підлогою кімнати
        if (body[1].y > (PLAY_Y_MAX - 17 - body[1].offY)) {
            body[1].y = PLAY_Y_MAX - 17 - body[1].offY;
            body[1].vy *= -0.1f; body[1].vx *= 0.6f; 
        }

        // ЖОРСТКА МАТРИЧНА СИНХРОНІЗАЦІЯ СУСТАВІВ ТА АВТОНОМНИЙ ПРОРАХУНОК КОЛІЗІЙ
        for (int i = 0; i < NODE_COUNT; i++) {
            if (i != 1) { 
                if (!body[i].isAmputated) {
                    float currentOffY = body[i].offY;
                    if (body[i].boneBroken) currentOffY += 6.0f; // Ефект зламаного провисання
                    
                    // ФІКС ІНДЕКСУ: Прив'язка до координат Торса через body[1]
                    body[i].x = body[1].x + body[i].offX;
                    body[i].y = body[1].y + currentOffY;
                    body[i].vx = body[1].vx;
                    body[i].vy = body[1].vy;
                } else {
                    // АМПУТОВАНА ЧАСТИНА
                    body[i].vy += GRAVITY;
                    body[i].x += body[i].vx; body[i].y += body[i].vy;
                    body[i].vx *= AIR_RESISTANCE;

                    float hH = body[i].h / 2.0f;
                    if (body[i].y > (PLAY_Y_MAX - hH)) {
                        body[i].y = PLAY_Y_MAX - hH; body[i].vy *= -0.15f; body[i].vx *= 0.6f;
                    }
                }
            }

            // ЗБРОЯ ТА РУЙНУВАННЯ ШАРІВ ПЛОТІ
            if (touched) {
                float hDX = propX - body[i].x; float hitDY = propY - body[i].y;
                float hitDist = sqrt(hDX * hDX + hitDY * hitDY);
                
                if (hitDist < 24) {
                    if (currentTool == TOOL_ROD) {
                        if (body[i].skinHP > 0) {
                            body[i].skinHP -= 6;
                            body[1].vx += (hDX / hitDist) * -1.6f; body[1].vy += (hitDY / hitDist) * -1.6f; // ФІКС ІНДЕКСУ [1]
                            emitBlood(body[i].x, body[i].y);
                            if (i == 1 && body[1].skinHP < 35 && random(0,10) > 6) triggerSpecificOrganDrop(body[1].x, body[1].y); // ФІКС ІНДЕКСУ [1]
                        }
                    }
                    else if (currentTool == TOOL_HAMMER && clickTriggered) {
                        body[i].boneBroken = true; body[i].skinHP -= 15;      
                        body[1].vx += (hDX / hitDist) * -5.0f; body[1].vy += (hitDY / hitDist) * -5.0f; // ФІКС ІНДЕКСУ [1]
                        emitBlood(body[i].x, body[i].y);
                    }
                    else if (currentTool == TOOL_KNIFE) {
                        emitBlood(body[i].x, body[i].y); body[i].skinHP = 0; 
                        if (i != 1 && clickTriggered) {
                            body[i].isAmputated = true; 
                            body[i].vx = random(-20, 21) / 10.0f; body[i].vy = random(-25, -5) / 10.0f;
                        }
                    }
                }
            }

            // ЗАКРІПЛЕННЯ КОЛІЗІЇ З ПІДЛОГОЮ ДЛЯ КОЖНОЇ ЧАСТИНИ
            float halfH = body[i].h / 2.0f;
            if (!body[i].isAmputated) {
                if (body[i].y > (PLAY_Y_MAX - halfH)) {
                    float depth = body[i].y - (PLAY_Y_MAX - halfH);
                    body[1].y -= depth; // ФІКС ІНДЕКСУ [1]: Виштовхуємо Торс вгору
                    body[1].vy *= -0.1f; 
                    body[1].vx *= 0.7f;  
                    if (abs(body[1].vy) > 1.5f) emitBlood(body[i].x, body[i].y);
                }
            }

            // Колізії зі стінами
            float hW = body[i].w / 2.0f;
            if (body[i].x < (PLAY_X_MIN + hW)) { if(i==1) body[1].x = PLAY_X_MIN + hW; else if(body[i].isAmputated) body[i].x = PLAY_X_MIN + hW; }
            if (body[i].x > (PLAY_X_MAX - hW)) { if(i==1) body[1].x = PLAY_X_MAX - hW; else if(body[i].isAmputated) body[i].x = PLAY_X_MAX - hW; }
        }
    }

    // Рендеринг нутрощів
    for (int i = 0; i < MAX_ORGANS; i++) {
        if (organPool[i].active) {
            organPool[i].vy += 0.25f; organPool[i].x += organPool[i].vx; organPool[i].y += organPool[i].vy; organPool[i].vx *= 0.97f;
            if (organPool[i].type == ORGAN_HEART)       buffer.fillRect((int)organPool[i].x - 2, (int)organPool[i].y - 2, 4, 4, COLOR_HEART);
            else if (organPool[i].type == ORGAN_LUNG)   buffer.fillRect((int)organPool[i].x - 3, (int)organPool[i].y - 2, 5, 3, COLOR_LUNG);
            else                                        buffer.drawFastHLine((int)organPool[i].x - 3, (int)organPool[i].y, 6, COLOR_INTESTINE);
            if (organPool[i].y > PLAY_Y_MAX - 3) { organPool[i].y = PLAY_Y_MAX - 3; organPool[i].vx = 0; organPool[i].vy = 0; }
        }
    }
    // Рендеринг крові
    for (int i = 0; i < MAX_BLOOD; i++) {
        if (bloodPool[i].active) {
            bloodPool[i].vy += 0.18f; bloodPool[i].x += bloodPool[i].vx; bloodPool[i].y += bloodPool[i].vy;
            if (bloodPool[i].x >= PLAY_X_MIN + 2 && bloodPool[i].x <= PLAY_X_MAX - 2 && bloodPool[i].y <= PLAY_Y_MAX - 2) buffer.fillCircle((int)bloodPool[i].x, (int)bloodPool[i].y, 1, AND_RED);
            if (bloodPool[i].y >= PLAY_Y_MAX - 2) { bloodPool[i].y = PLAY_Y_MAX - 2; bloodPool[i].vx = 0; bloodPool[i].vy = 0; if (random(0, 100) > 96) bloodPool[i].active = false; }
        }
    }

    // Тришаровий рендеринг манекена
    if (dollExists) {
        for (int i = 0; i < NODE_COUNT; i++) {
            if (i != 1 && !body[i].isAmputated) {
                buffer.drawLine((int)body[1].x, (int)body[1].y, (int)body[i].x, (int)body[i].y, body[i].boneBroken ? AND_RED : AND_TEXT3); // ФІКС ІНДЕКСУ [1]
            }
        }
        for (int i = 0; i < NODE_COUNT; i++) {
            int xb = (int)body[i].x - (body[i].w / 2); int yb = (int)body[i].y - (body[i].h / 2);
            buffer.fillRect(xb, yb, body[i].w, body[i].h, AND_RED); 
            if (body[i].skinHP > 35) { int pW = (body[i].w * body[i].skinHP) / 100; int pH = (body[i].h * body[i].skinHP) / 100; buffer.fillRect(xb + (body[i].w - pW)/2, yb + (body[i].h - pH)/2, pW, pH, AND_PINK); } 
            if (body[i].skinHP > 75) { int sW = (body[i].w * body[i].skinHP) / 100; int sH = (body[i].h * body[i].skinHP) / 100; buffer.fillRect(xb + (body[i].w - sW)/2, yb + (body[i].h - sH)/2, sW, sH, AND_TEXT); } 
            buffer.drawRect(xb, yb, body[i].w, body[i].h, AND_TEXT3);
        }
        int hX = (int)body[0].x; int hY = (int)body[0].y; // ФІКС ОЧЕЙ ПО ГОЛОВІ [0]
        buffer.fillRect(hX - 5, hY - 3, 3, 3, AND_BG); buffer.fillRect(hX + 2, hY - 3, 3, 3, AND_BG); 
    }

    // Оновлення пропів зброї
    if (touched && tx >= PLAY_X_MIN) {
        if (currentTool == TOOL_ROD)         buffer.drawLine((int)propX - 12, (int)propY - 12, (int)propX + 12, (int)propY + 12, COLOR_ROD);
        else if (currentTool == TOOL_HAMMER) { buffer.fillRect((int)propX - 6, (int)propY - 6, 12, 10, COLOR_HAMMER); buffer.drawLine((int)propX, (int)propY + 4, (int)propX, (int)propY + 16, COLOR_ROD); }
        else if (currentTool == TOOL_KNIFE)  { buffer.drawLine((int)propX - 10, (int)propY + 10, (int)propX + 10, (int)propY - 10, COLOR_ROD); buffer.fillCircle((int)propX - 10, (int)propY + 10, 3, AND_RED); }
    }

    // Сайдбар меню
    buffer.fillRect(0, 0, SIDEBAR_W, SCREEN_HEIGHT, AND_SURFACE);
    buffer.drawFastVLine(SIDEBAR_W, 0, SCREEN_HEIGHT, AND_TEXT3);

    buffer.fillRect(5, 5, 40, 30, (currentTool == TOOL_SPAWN) ? AND_ACCENT : AND_SURFACE3);
    buffer.drawRect(5, 5, 40, 30, AND_TEXT3);
    buffer.fillRect(19, 14, 12, 12, AND_TEXT); buffer.drawRect(19, 14, 12, 12, AND_TEXT3);

    buffer.fillRect(5, 42, 40, 30, (currentTool == TOOL_DRAG) ? AND_ACCENT : AND_SURFACE3);
    buffer.drawRect(5, 42, 40, 30, AND_TEXT3);
    buffer.setTextColor(AND_TEXT); buffer.setTextSize(1); buffer.setTextDatum(MC_DATUM); buffer.drawString("DRAG", 25, 57);

    buffer.fillRect(5, 79, 40, 30, (currentTool == TOOL_ROD) ? AND_ACCENT : AND_SURFACE3);
    buffer.drawRect(5, 79, 40, 30, AND_TEXT3); buffer.drawString("ROD", 25, 94);

    buffer.fillRect(5, 116, 40, 30, (currentTool == TOOL_HAMMER) ? AND_ACCENT : AND_SURFACE3);
    buffer.drawRect(5, 116, 40, 30, AND_TEXT3); buffer.drawString("HAMR", 25, 131);

    buffer.fillRect(5, 153, 40, 30, (currentTool == TOOL_KNIFE) ? AND_ACCENT : AND_SURFACE3);
    buffer.drawRect(5, 153, 40, 30, AND_TEXT3); buffer.drawString("KNIF", 25, 168);

    buffer.fillRect(5, 190, 40, 30, AND_RED); buffer.drawRect(5, 190, 40, 30, AND_TEXT3); buffer.drawString("MENU", 25, 205);

    buffer.pushSprite(0, 0);
    delay(10);
}
