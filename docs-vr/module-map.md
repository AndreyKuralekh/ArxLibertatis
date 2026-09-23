# Карта модулей Arx Libertatis для VR

Состояние: ветка `vr` = `upstream/master` `5b95e4c` (1.3-dev), 2026-09-24.
Пути — от `src/`. Номера строк ориентировочные (сдвигаются при слияниях с upstream), ищите по имени функции.

## Главное (что определяет архитектуру этапа 1)

1. **Рендер — фиксированный конвейер OpenGL 1.5 (или 2.0 compat), без шейдеров и без FBO.** В коде нет ни одного
   `glBindFramebuffer`: всё рисуется в back buffer окна. Для OpenXR нужно добавить в `OpenGLRenderer` рендер в текстуру
   (FBO на образы swapchain) и запрашивать контекст ≥ 3.x compatibility.
2. **Две ветви геометрии.**
   - Комнаты/фон (`SMY_VERTEX3`, VBO комнат) трансформируются на GPU матрицами `m_view`/`m_projection`.
   - **Всё остальное проецируется на CPU в пиксели экрана:** NPC, предметы, руки и оружие игрока, частицы,
     эффекты заклинаний, тени, ореолы, блики. Это `TexturedVertex` через `worldToClipSpace()` (`graphics/data/Mesh.cpp:419`)
     с `g_preparedCamera.m_worldToScreen`. Рисуется в «2D-режиме» `disableTransform()`.
   - Значит, **стерео = вызвать `PrepareCamera()` на каждый глаз и прогнать отрисовку дважды.**
3. **Обновление и отрисовка перемешаны.** `ARX_PARTICLES_Update`, `ParticleSparkUpdate`, `ARX_MAGICAL_FLARES_Update`,
   `ARX_SPELLS_Update`, `g_particleManager.Render` одновременно двигают симуляцию и рисуют. Второй вызов на глаз
   ускорит эффекты вдвое. Варианты:
   - (A) разделить update/draw;
   - (B) на время второго прохода подменять dt = 0;
   - (C) перевести `TexturedVertex` на мировые координаты с GPU-трансформацией.
   Для этапа 1 разумно (A) или (B).
4. **2D и 3D различаются не по типу вершин, а по месту вызова.** UI начинается в `ArxGame::renderLevel()` примерно
   со строки 1845. Отсюда разрез: мир — по глазам, UI — один раз в отдельный FBO.
5. **Весь UI раскладывается по `g_size`** (размер окна). Для UI в текстуре выставить `g_size`/`g_sizeRatio`
   = размер UI-FBO и вызвать путь ресайза (`AdjustUI`, `MenuReInitAll`, `g_hudRoot.recalcScale`).
6. **Единицы: 1 ед. ≈ 1 см, ось Y направлена вниз.** Рост игрока 170, присед 120, радиус 52.
   Near plane зашит как 1.0 (`game/Camera.cpp:52`), far = `cam->cdepth` (обычно 2100, меняется туманом зоны).
7. **Геймпада нет**, только клавиатура и мышь. Виртуальную мышь лучше подавать через `InputBackend`.

---

## 1. Главный цикл и кадр

| Что | Где |
|---|---|
| Цикл | `core/ArxGame.cpp` `ArxGame::run()` (~1072): `doFrame()` + `processEvents()` |
| Кадр | `ArxGame::doFrame()` (~1096): ограничитель FPS (sleep) → `updateTime()` → `updateInput()` → ресайз → `HandleGameFlowTransitions()` (логотипы, свой `showFrame`) → загрузка уровня/быстрое сохранение → `render()` → `m_MainWindow->showFrame()` (~1215) |
| Present | `window/SDL2Window.cpp` `SDL2Window::showFrame()` (~840) → `SDL_GL_SwapWindow` |
| VSync | `SDL2Window::setVSync()` (~599), из `config.video.vsync` в `ArxGame::initWindow` |
| Свои `showFrame` | `gui/LoadLevelScreen.cpp:~117` (экран загрузки), логотипы, `gui/MainMenu.cpp:~849` (смена видеорежима), `ArxGame::onRendererInit` |
| Скриншот | `OpenGLRenderer::getSnapshot` (~862): `glReadPixels` из окна, для миниатюр сохранений (`ArxGame.cpp:~1203`, `gui/Menu.cpp:~181`) |

**Для VR:**
- Обернуть `render()` в `xrWaitFrame`/`xrBeginFrame` … `xrEndFrame`. `xrWaitFrame` заменяет sleep ограничителя FPS.
- VSync окна выключить, окно оставить как зеркало (опционально).
- Экран загрузки и логотипы тоже должны отправлять кадры OpenXR (quad-слой), иначе рантайм сочтёт приложение зависшим.
- `getSnapshot` должен читать из FBO/зеркала.

## 2. Окно, контекст OpenGL, инициализация рендера

| Что | Где |
|---|---|
| Создание окна | `ArxGame::initWindow()` (~385–441): сначала SDL2, потом SDL1; `setMaxMSAALevel`, `setVSync` |
| Атрибуты GL | `SDL2Window::initialize()` (~361–590): desktop — 2.0 compat (режим no-error) или 1.5 legacy («core profile not supported yet», ~398); GLES 1.0 |
| MSAA и окно | `SDL2Window::createWindowAndGLContext()` (~252): перебор MSAA 8→0, depth 24/16 |
| `--override-gl` | `graphics/opengl/OpenGLUtil.cpp:43`: влияет только на определение возможностей, не на создание контекста |
| Инициализация рендера | `OpenGLRenderer::initialize()` (~91): epoxy/GLEW, минимум GL 1.5; `reinit()` (~365): сброс состояния, `onRendererInit` → `ArxGame::onRendererInit` (~2012): динамические VBO |
| Viewport/scissor | `OpenGLRenderer::SetViewport` (~570), `SetScissor` (~589): переворачивают Y **по высоте окна** |
| Режимы трансформации | `enableTransform()` (~470, матрицы вида/проекции), `disableTransform()` (~489, пиксели → NDC по viewport) |
| Размер экрана | `core/Core.cpp:~201–238` `g_size`, `g_sizeRatio` (относительно 640×480) |

**Для VR:**
- После создания GL-контекста создать `XrSession` с `XrGraphicsBindingOpenGLWin32KHR` (HDC/HGLRC из SDL/WGL).
- Запросить контекст 3.x+ compatibility, чтобы fixed-function продолжил работать.
- Добавить `OpenGLRenderer::setRenderTarget(fbo, size)`: `SetViewport`/`SetScissor`/`getSnapshot` должны брать высоту
  цели, а не окна.
- MSAA в FBO — через multisample renderbuffer + resolve.

## 3. Камера

| Что | Где |
|---|---|
| `struct Camera` | `game/Camera.h:29`: `m_pos`, `angle` (pitch/yaw/roll, градусы), `focal` (FOV через фокус при высоте 480), `cdepth` (far) |
| `struct PreparedCamera` | `Camera.h:~94`: `m_worldToView`, `m_viewToClip`, `m_viewToScreen`, `m_worldToScreen`, `m_viewToWorld` |
| Глобальные | `game/Camera.cpp:28–32`: `g_camera`, `g_playerCamera`, `g_preparedCamera`, `g_cameraEntity`, `g_playerCameraStablePos` |
| Проекция | `createProjectionMatrix()` (`Camera.cpp:48`): near = 1, far = cdepth, глубина [0,1] в стиле D3D, Y перевёрнут, симметричная + `centerShift` |
| **Единственная точка загрузки матриц** | `PrepareCamera(cam, viewport, center)` (`Camera.cpp:78`): view = `toRotationMatrix(angle)` × translate(−pos) (`graphics/Math.cpp:153`), `SetViewMatrix`/`SetProjectionMatrix`/`SetViewport` |
| Камера от тела | `ArxGame::updateFirstPersonCamera()` (`ArxGame.cpp:~1220`): позиция = вершина `view_attach` модели игрока (с покачиванием от анимации) или `g_playerCameraStablePos`; не дальше 46 ед. от `player.pos`; угол = `player.angle` |
| Выбор активной | `updateActiveCamera()` (~1469): камера скрипта или игрока, `ManageQuakeFX`, `PrepareCamera(cam, g_size)` |
| FOV | `g_playerCamera.setFov(config.video.fov)` + зум магического зрения и прицела лука (`focal += …`, ~1765–1777) |
| Отсечение | `scene/Scene.cpp` `CreateScreenFrustrum()` (~891) из матриц камеры; порталы `ARX_PORTALS_Frustrum_ComputeRoom` (~1573) |
| Другие вызовы `PrepareCamera` | книга (`gui/book/Book.cpp:~1083`, персонаж), ожерелье рун (`Necklace.cpp:~167`), кинематика (`cinematic/Cinematic.cpp:~319`) |

**Для VR:**
- Камера = тело (`player.pos` + yaw) + поза головы из `xrLocateViews` (переводить в см, Y вниз).
- Вариант `PrepareCamera` с явной матрицей вида и асимметричной проекцией из `XrFovf`.
- Near ≈ 5–10 ед. или оставить 1.
- Frustum для отсечения — объединение двух глаз, считать один раз за кадр.
- Отключить зум по `focal`, `ManageQuakeFX` (roll) и `viewBobbing`.
- Книга и ожерелье после своего 3D должны восстанавливать камеру глаза, а не `g_size`.

## 4. Порядок отрисовки кадра

`ArxGame::render()` (~1894): `ARX_PLAYER_Frame_Update`, `manageKeyMouse`, пикинг `FlyingOverObject` → одна из ветвей:
- меню / создание персонажа / титры: `ARX_Menu_Render()` (`gui/Menu.cpp:228`), только 2D;
- кинематика: `cinematicRender()` → `Cinematic::Render`;
- игра: `updateLevel()` + `renderLevel()`.

После ветви — отладочный HUD и консоль (`g_console.draw()`).

`updateLevel()` (~1601), в основном симуляция: управление → движение игрока → анимации → бой → камера → `PrepareCamera`
→ физика → `ARX_SCENE_Update()` (frustum, свет, порталы) → `g_particleManager.Update` → туман → захват точек рун
→ `ARX_SPELLS_ManageMagic` → FOV.

`renderLevel()` (~1782):
1. `Clear(color|depth, fogColor)`, полосы кинематики, AA, туман (`SetFogParams`).
2. `ARX_SCENE_Render()` (`scene/Scene.cpp:~1588`):
   - `BackgroundRenderOpaque` по комнатам (GPU);
   - тени-блобы;
   - брошенные предметы;
   - `RenderInter()` (NPC/предметы, CPU);
   - `PopAllTriangleListOpaque`;
   - **игрок от первого лица** `AnimatedEntityRender(player)`, сначала без depth test;
   - глаз-шпион;
   - декали `PolyBoomDraw`;
   - прозрачные треугольники;
   - прозрачный фон;
   - вода и лава;
   - `Halo_Render`.
3. Отладочная отрисовка.
4. Частицы: `g_particleManager.Render`, `ARX_PARTICLES_Update`, `ParticleSparkUpdate` (обновление + отрисовка).
5. `ARX_MAGICAL_FLARES_Update`.
6. Экранные эффекты: improve vision, magic sight, паралич, `ARX_DAMAGE_Show_Hit_Blood`.
7. `ARX_SPELLS_Update()` (эффекты заклинаний) → `g_renderBatcher.render()`.
8. Блики `updateLightFlares`/`renderLightFlares` (в экранном пространстве).
9. Смерть игрока.
10. **С ~1845 — 2D:**
    - `ARX_INTERFACE_NoteManage`, `g_hudRoot.draw()`, блики книги;
    - `Clear(depth)`;
    - `notification_check`, `pTextManage->Render`;
    - миникарта;
    - курсор `ARX_INTERFACE_RenderCursor`;
    - `ManageFade`;
    - субтитры `ARX_SPEECH_Update`.

**Для VR:** `renderLevel` делится на три части:
- (a) обновление эффектов, один раз;
- (b) 3D-проход на каждый глаз в FBO swapchain (шаги 1–8, экранные эффекты — как наложение, привязанное к голове);
- (c) UI-проход один раз в UI-FBO (шаг 10 + консоль + меню + кинематика) → `XrCompositionLayerQuad`.

Экранные блики переделать на каждый глаз или отключить. Тело игрока «без depth test» в VR будет выглядеть неправильно:
позже оставить только руки/оружие на контроллерах.

## 5. 2D-интерфейс

| Элемент | Где | Как рисуется |
|---|---|---|
| Примитивы | `graphics/Draw.cpp:57–200` `EERIEDrawBitmap*`, `EERIEDRAWPRIM`; `render2D()` (`graphics/Renderer.h:~489`) | `TexturedVertex` в пикселях, динамический VBO |
| Текст | `gui/Text.cpp`: шрифты `hFont*` (размер от `g_sizeRatio` × `config.interface.fontSize`, `ARX_Text_Init` ~356); `ARX_UNICODE_DrawTextInRect` (~214), `drawTextCentered`; `drawTextAt` (~501) проецирует мировую точку | глифы → `TexturedVertex` (`graphics/font/Font.cpp`) |
| HUD | `gui/Hud.cpp` `HudRoot::draw()` (~1376): здоровье/мана, сила удара, скрытность, быстрые слоты, активные заклинания, иконки; привязка к `Rectf(g_size)` (`gui/hud/HudCommon.cpp`) | масштаб `config.interface.hudScale`, `getInterfaceScale()` (`gui/Interface.cpp:~1939`) |
| Инвентарь | `gui/hud/PlayerInventory.cpp` (внизу по центру `g_size`), `SecondaryInventory.cpp` | — |
| Книга | `gui/book/Book.cpp` (`g_bookRect` по центру `g_size`, `bookScale`); **внутри есть 3D**: персонаж и ожерелье рун через `PrepareCamera` + scissor | — |
| Меню | `gui/Menu.cpp` `ARX_Menu_Render`/`ARX_Menu_Manage`, `gui/MenuWidgets.cpp`, `gui/MainMenu.cpp`, `gui/menu/*`, `gui/widget/*`; фон на весь экран; мышь — напрямую `GInput->getMousePosition()` | часть окон смешивается с фоном (`BlendZero, BlendInvSrcColor` в `MenuWidgets.cpp:~254`, `MenuFader`) |
| Курсор | `gui/Cursor.cpp` `ARX_INTERFACE_RenderCursor` (~246): рисуется в `DANAEMouse`; прицел в `g_size.center()`; `gui/menu/MenuCursor.cpp` | — |
| Субтитры | `gui/Speech.cpp` `ARX_SPEECH_Update` (~257): полоса внизу `g_size` | — |
| Уведомления/описания | `gui/Notification.cpp`, `gui/TextManager.cpp`, `ArxGame::manageEntityDescription` (`gui/Interface.cpp:~1602`) | — |
| Записки | `gui/Note.cpp`, `ARX_INTERFACE_NoteManage` (`gui/Interface.cpp:~469`) | — |
| Загрузка/логотипы | `gui/LoadLevelScreen.cpp` (свой `showFrame`), `HandleGameFlowTransitions` | — |
| Кинематика | `cinematic/CinematicController.cpp:157`, `Cinematic::Render` (Clear, scissor 4:3, своя камера); `gui/CinematicBorder.cpp` (полосы через Clear) | — |
| 2D-частицы | `PARTICLE_2D`: огонь факела на HUD (`gui/Hud.cpp:~529`), `MagFX`, след рун (`graphics/particle/MagicFlare.cpp`) | рисуются **внутри 3D-прохода** |

**Для VR:**
- Сначала весь UI целиком в один UI-FBO (например 1280×960) с `g_size` = размер FBO, показ как quad-слой в мире.
- Смешивание с фоном (меню, fader, курсор) в прозрачной текстуре будет выглядеть иначе: FBO очищать непрозрачным
  тёмным цветом или переделать режимы смешивания.
- 2D-частицы вынести в UI-проход.
- Субтитры позже — на отдельную панель, привязанную к голове.
- Кинематику — на «экран-кинотеатр».

## 6. Ввод

| Что | Где |
|---|---|
| Бэкенд | `input/InputBackend.h` (абстрактный: мышь абс./отн., кнопки, колесо, клавиши, текстовый ввод); `input/SDL2InputBackend.cpp` (`onEvent` ~410, `update` ~314, `SDL_SetRelativeMouseMode`, `SDL_WarpMouseInWindow`) |
| `Input` (`GInput`) | `input/Input.cpp`: `update()` (~435–597), позиция мыши **ограничена размером окна** (~538–550), чувствительность/инверсия, двойной клик 300 мс; `setMouseMode` (Absolute/Relative) |
| Старые флаги кликов | `EERIEMouseButton`, `eeMousePressed1()` и т.п. (`input/Input.h:67–95`), заполняются в `ArxGame::updateInput()` (`ArxGame.cpp:~1504`) из действий ACTION/USE |
| Действия | `enum ControlAction` (`core/Config.h:37–83`), `ActionKey{key[2]}`; `Input::actionNowPressed/actionPressed/actionNowReleased` (`Input.cpp:~828–1003`) |
| Мышь → игра | `ArxGame::manageKeyMouse()` (`gui/Interface.cpp:1254`): пикинг, mouselook (Relative/Absolute, `MemoMouse`), **`DANAEMouse = GInput->getMousePosition()` (~1410)**, поворот `player.desiredangle` (~1563–1596), поворот у края экрана |
| Клавиши → игрок | `ArxGame::managePlayerControls()` (`gui/Interface.cpp:642`): ходьба/стрейф/присед/наклон/прыжок/магия/книга/предзаклинания/оружие → `player.m_currentMovement`, `g_moveto` |
| Предметы/мир | `ArxGame::manageEditorControls()` (~1658): `eMouseState`, перетаскивание (`gui/Dragging.cpp`, `screenToWorldSpace`), `InterClick`/`GetFirstInterAtPos` (`scene/Interactive.cpp:~1627–1712`) по экранному `bbox2D` |
| Пикинг | `FlyingOverObject(DANAEMouse)` (`core/Core.cpp:258`): сначала прямоугольники HUD, потом сущности |
| Джойстик/геймпад | **нет** |
| Текстовый ввод | `input/TextInput.cpp` (имена сохранений, консоль) → в VR нужна виртуальная клавиатура |

**Для VR:**
- **Виртуальная мышь** — через декоратор `InputBackend` (`VRInputBackend`): координаты точки на UI-панели,
  курок = `Button_0`, grip = `Button_1`, стик = колесо. Подключается в `Window::getInputBackend` / `Input::init`.
  Ограничение по размеру окна заменить на размер UI-FBO, warp превратить в no-op.
- **Действия** (ходьба, поворот, прыжок, присед, магия, книга, инвентарь) — синтетические клавиши или массив
  переопределений, который проверяют `actionPressed`/`actionNowPressed` в первую очередь.
- Mouselook в VR не используется: вращение даёт голова. Режим Relative не включать, `borderTurning` отключить.
- Пикинг мира — луч от контроллера вместо `bbox2D` и центра экрана.

## 7. Игрок

| Что | Где |
|---|---|
| Состояние | `struct ARXCHARACTER` (`game/Player.h:236–406`), `player`: `pos` (верх цилиндра ≈ голова), `angle`, `desiredangle`, `physics.cyl`, `m_currentMovement` (`PLAYER_MOVE_*`, `PLAYER_CROUCH`, `PLAYER_LEAN_*`), `jumpphase`, `levitate` |
| Размеры | `Player.h:385–398`: `baseHeight()=-170`, `crouchHeight()=-120`, радиус 52; `basePosition()` = ноги |
| Движение | `managePlayerControls` строит `g_moveto` (`gui/Interface.cpp:~800–886`) → `ARX_PLAYER_Manage_Movement()` (`game/Player.cpp:~2233`) → `PlayerMovementIterate` (~1787): земля/падение, **скорость масштабируется анимацией** (~1959–1987), гравитация, прыжок, `ARX_COLLISION_Move_Cylinder` |
| Коллизии | `physics/Collisions.cpp`: `ARX_COLLISION_Move_Cylinder` (~1130), `CheckAnythingInCylinder` (~614), `AttemptValidCylinderPos` (~1010) |
| Присед | высота цилиндра меняется после анимации `ANIM_CROUCH_START` (`Player.cpp:~1458–1504`) |
| Поворот тела | `ARX_PLAYER_Frame_Update` (`Player.cpp:~1628`): `angle = desiredangle`, pitch → кости позвоночника/головы; модель поворачивается только по yaw |
| Камера на теле | см. п. 3 `updateFirstPersonCamera`; скрытие головы — `ARX_INTERACTIVE_Show_Hide_1st` |

**Для VR:**
- Стик → вектор `tm` в `managePlayerControls`, повёрнутый по yaw головы/контроллера. Флаги `PLAYER_MOVE_*`
  сохранить: от них зависят анимация и скорость.
- Snap/плавный поворот → `player.desiredangle.setYaw`.
- Room-scale: смещение головы добавлять в `g_moveto`, чтобы оно проходило через коллизии; `player.pos` не писать напрямую.
- Физический присед: высота головы ниже порога → `PLAYER_CROUCH`.
- Pitch игрока держать 0 (голова даёт pitch камере).

## 8. Бой

| Что | Где |
|---|---|
| Вход | `eeMousePressed1()` ← `CONTROLS_CUST_ACTION`; режим боя — `ARX_INTERFACE_setCombatMode` (`gui/Interface.cpp:593`) |
| Зажать/отпустить | `ManageCombatModeAnimations()` (`core/Core.cpp:~496–947`): WAIT + нажатие → STRIKE_START (направление = `m_strikeDirection` от последней клавиши движения) → CYCLE (копится `m_aimTime`) → отпускание → STRIKE; в окне 30–70 % анимации `ARX_EQUIPMENT_Strike_Check` |
| Сила | `StrikeAimtime()` (~472): `m_strikeAimRatio = clamp(m_aimTime / Full_AimTime, 0.1, 1)`; `Full_AimTime` (`game/Player.cpp:~416`) |
| Удар | `game/Equipment.cpp` `ARX_EQUIPMENT_Strike_Check` (~597): сферы по точкам `hit_NN` оружия → `CheckEverythingInSphere` → `ARX_EQUIPMENT_ComputeDamages` (~427); кулаки — сфера 25 у `primary_attach` (`Core.cpp:~550`) |
| Лук | `Core.cpp:~741–943`: CYCLE копит `m_bowAimRatio` (зум FOV), отпускание → `ARX_THROWN_OBJECT_Throw`; `config.input.improvedBowAim` — направление от кости руки |

**Для VR:**
- Этап 1: курок = ACTION, механика «зажми и отпусти» работает без изменений.
- Этап 3: VR-ветка в `ManageCombatModeAnimations`: оружие следует за позой контроллера, `Strike_Check` вызывается
  при скорости замаха выше порога, сила = скорость. Событие `SM_STRIKE` для скриптов сохранить.
- Лук: ветка `improvedBowAim` с позой контроллера.

## 9. Магия

| Что | Где |
|---|---|
| Захват точек | `ArxGame.cpp:~1730–1753`: пока нажата ACTION в режиме магии — **`ARX_SPELLS_AddPoint(DANAEMouse)`** каждые 16 мс. **Единственная точка входа 2D-точек в распознаватель** (`Vec2s`, пиксели окна, Y вниз) |
| Режим магии | `ARX_SPELLS_ManageMagic()` (`game/Spells.cpp:~441`): удерживается `CONTROLS_CUST_MAGICMODE`, эффекты `AddFlare` (`graphics/particle/MagicFlare.cpp`, мир через `screenToWorldSpace`); отпустил кнопку → анализ руны; отпустил режим → `ARX_SPELLS_AnalyseSPELL` |
| Распознаватель | `game/magic/SpellRecognition.cpp`: `plist` (≤200 точек), `ARX_SPELLS_AddPoint` (594); по умолчанию `useAltRuneRecognition`: `ARX_SPELLS_Analyse_Alt` (ключевые точки → 8 направлений → шаблоны `patternData`, не зависит от масштаба); старый `ARX_SPELLS_Analyse` зависит от пикселей |
| Запуск заклинания | `ARX_SPELLS_Launch` (`Spells.cpp:~826`), предзаклинания `game/magic/Precast.cpp` |

**Для VR:**
- Распознаватель не трогать. Кончик контроллера проецировать на плоскость перед HMD (~0.5 м) в виртуальное
  2D-пространство ~640×480 (Y вниз) и подавать в `ARX_SPELLS_AddPoint` вместо `DANAEMouse`.
- Огоньки (flares) — в 3D у кончика контроллера.
- Режим магии = grip, рисование = курок.

## 10. Конфиг, командная строка, CMake

| Что | Где |
|---|---|
| Конфиг | `core/Config.h:119–263` `class Config` (структуры по секциям); `core/Config.cpp`: `namespace Default` (~53), `Section` (~188), `Key` (~200), `save()` (~445), `init()` (~575). Файл `cfg.ini` в `fs::getConfigDir()` |
| Новая секция `[vr]` | `struct {…} vr;` в `Config.h` + `Default`/`Section::Vr`/`Key` + запись в `save()` + чтение в `init()` |
| Параметры командной строки | макросы `ARX_PROGRAM_OPTION`/`_ARG` (`platform/ProgramOptions.h:128`), саморегистрация в любом .cpp; примеры `--skiplogo` (`core/ArxGame.cpp:~558`), `--data-dir` (`io/fs/SystemPaths.cpp:~526`). Разбор (`core/Startup.cpp:137`) идёт **до** загрузки конфига → `--vr` ставит статический флаг, который потом перекрывает `config.vr.enabled` |
| Пути | `io/fs/SystemPaths.cpp`: user/config/data dirs, `getSearchPaths` (~254); лог `getUserDir()/arx.log` |
| CMake | единый корневой `CMakeLists.txt`: опции (27–173) → добавить `option(ARX_VR … OFF)`; поиск зависимостей (~412–470) → `find_package(OpenXR)` (модули в `cmake/`); списки исходников `set(X_SOURCES …)` (~897–1255) → `PLATFORM_XR_SOURCES`; условные добавления (~1263–1396); `src/Configure.h.in` → `#cmakedefine01 ARX_HAVE_OPENXR`; `print_configuration("Features" …)` (~1987). Учитывать `UNITY_BUILD`: конфликты `static`-имён в новых файлах |
| Меню настроек | `gui/MainMenu.cpp` (пример чекбокса: mouse look ~1430) |
