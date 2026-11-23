#pragma once
// Interpolação dentro do jogo (alternativa 3). Plano: estudo/plano_interpolacao.md
namespace InterpRec
{
// Modo foto: a free camera for the last picture (the game is paused); view = 3x4 matrix that multiplies the game's camera
struct Photo { bool on = false; bool hideHud = true; float zoom = 1.0f; float view[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } }; };
void SetPhoto(const Photo& p);
bool PhotoFrame();                 // redraws the picture on the screen with the photo camera and shows it (false: nothing recorded)
bool PhotoAvailable();             // there is a recorded picture to redraw (needs the interpolation on)
void SetAutoBudget(bool on);       // menu: lower the extra pictures when the game falls behind (on) or never (off)
void Configure(int fps);           // escolha do menu: 0 = desligada, 60..400 = FPS (vale no próximo renderizador)
void Install();                    // (re)liga o gravador no rasterizador atual
void OnFrameEnd(bool newImage);    // fim de cada quadro apresentado (newImage = o jogo entregou imagem nova)
bool TakeExtraFlag();              // 60 FPS: o swap que acabou de acontecer mostrou um quadro do meio
double TakeShowAt();               // acima de 60: > 0 = o quadro que acabou de ser apresentado é do meio, mostrar nessa hora
bool SuppressNormal();             // acima de 60: a interpolação está ativa, o quadro normal do VBlank não aparece
unsigned long long VblankCount();  // (diagnóstico) VBlanks vistos pela interpolação
}
