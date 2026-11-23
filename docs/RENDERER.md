# O renderer do jogo (N3)

Mapa das funções do jogo que escrevem comandos da GPU (RSX) no buffer de comandos, gerado por
`scripts/analysis/gcm_map.py` a partir do código traduzido das duas edições (23/11). É o ponto de partida da Parte 15,
N3, do `PLANO0.6.1.md`: as funções do nível mais alto que um renderer nativo substitui.

## Como o jogo monta os comandos

O jogo escreve os comandos direto no buffer: cada cabeçalho é `(count << 18) | método`, montado no PPU com `lis`
(o `count`) e `ori` (o método NV4097). A ferramenta acha esse par no C++ traduzido. Não pega métodos calculados a
partir de um índice (texturas por unidade, por exemplo) nem os comandos que a `libgcm` monta: esses ficam para a
próxima versão do mapa.

## Resultado

De 18.864 funções traduzidas na normal (19.376 na Premium), **32 escrevem comandos**, nas duas edições as mesmas:
3 de desenho, e o resto de passe (alvos de render, limpeza, viewport) e de material (shaders, blend, stencil, alpha,
cull). Os desenhos do jogo inteiro passam por **3 funções**. "0 chamadores" quer dizer chamada só por ponteiro
(tabela virtual).

| Camada | Normal (BLJS10220) | Premium (BLJS10219) | Chunk | Chamadores diretos | Métodos |
|---|---|---|---|---|---|
| draw | `func_00456548` | `func_00457DC8` | 015 | 9 | `DRAW_ARRAYS`, `SET_BEGIN_END` |
| draw | `func_00456B38` | `func_004583B8` | 015 | 4 | `DRAW_INDEX_ARRAY`, `SET_BEGIN_END`, `CLEAR_SURFACE`, `SET_COLOR_MASK`, `SET_CONTEXT_DMA_COLOR_A`, `SET_CONTEXT_DMA_COLOR_B` e mais 7 |
| draw | `func_005B4718` | `func_005B5F90` | 020 | 1 | `DRAW_INDEX_ARRAY`, `SET_BEGIN_END`, `SET_INDEX_ARRAY_ADDRESS` |
| pass | `func_0009FCC4` | `func_0009FCC4` | 002 | 0 | `SET_SCISSOR_HORIZONTAL` |
| pass | `func_000A0F0C` | `func_000A0F0C` | 002 | 1 | `SET_CONTEXT_DMA_COLOR_A`, `SET_CONTEXT_DMA_COLOR_B`, `SET_CONTEXT_DMA_ZETA`, `SET_SURFACE_CLIP_HORIZONTAL`, `SET_SURFACE_COLOR_TARGET`, `SET_SURFACE_FORMAT` e mais 2 |
| pass | `func_00173C08` | `func_001750C8` | 005 | 0 | `SET_SURFACE_COLOR_TARGET` |
| pass | `func_0017BDCC` | `func_0017D28C` | 005 | 1 | `SET_SURFACE_COLOR_AOFFSET` |
| pass | `func_003F4AC8` | `func_003F6348` | 014 | 0 | `SET_SURFACE_CLIP_HORIZONTAL` |
| pass | `func_004557B8` | `func_00457038` | 015 | 0 | `SET_CLIP_MIN`, `SET_VIEWPORT_HORIZONTAL` |
| pass | `func_004557C8` | `func_00457048` | 028 | 1 | `SET_CLIP_MIN`, `SET_VIEWPORT_HORIZONTAL` |
| pass | `func_0045582C` | `func_004570AC` | 028 | 8 | `SET_CLIP_MIN`, `SET_VIEWPORT_HORIZONTAL` |
| pass | `func_0045716C` | `func_004589EC` | 028 | 3 | `CLEAR_SURFACE`, `SET_COLOR_MASK`, `SET_CONTEXT_DMA_COLOR_A`, `SET_CONTEXT_DMA_COLOR_B`, `SET_CONTEXT_DMA_ZETA`, `SET_SURFACE_CLIP_HORIZONTAL` e mais 4 |
| pass | `func_004571B4` | `func_00458A34` | 029 | 3 | `SET_COLOR_MASK`, `SET_CONTEXT_DMA_COLOR_A`, `SET_CONTEXT_DMA_COLOR_B`, `SET_CONTEXT_DMA_ZETA`, `SET_SURFACE_CLIP_HORIZONTAL`, `SET_SURFACE_COLOR_TARGET` e mais 3 |
| pass | `func_004571F8` | `func_00458A78` | 029 | 3 | `SET_COLOR_MASK`, `SET_CONTEXT_DMA_COLOR_A`, `SET_CONTEXT_DMA_COLOR_B`, `SET_CONTEXT_DMA_ZETA`, `SET_SURFACE_CLIP_HORIZONTAL`, `SET_SURFACE_COLOR_TARGET` e mais 3 |
| pass | `func_004575A0` | `func_00458E20` | 015 | 2 | `CLEAR_SURFACE`, `SET_COLOR_CLEAR_VALUE` |
| pass | `func_0045926C` | `func_0045AAEC` | 015 | 0 | `SET_COLOR_MASK`, `SET_CONTEXT_DMA_COLOR_A`, `SET_CONTEXT_DMA_COLOR_B`, `SET_CONTEXT_DMA_ZETA`, `SET_SURFACE_CLIP_HORIZONTAL`, `SET_SURFACE_COLOR_TARGET` e mais 3 |
| pass | `func_00459270` | `func_0045AAF0` | 029 | 3 | `SET_COLOR_MASK`, `SET_CONTEXT_DMA_COLOR_A`, `SET_CONTEXT_DMA_COLOR_B`, `SET_CONTEXT_DMA_ZETA`, `SET_SURFACE_CLIP_HORIZONTAL`, `SET_SURFACE_COLOR_TARGET` e mais 3 |
| pass | `func_00459D44` | `func_0045B5C4` | 015 | 0 | `CLEAR_SURFACE`, `SET_COLOR_CLEAR_VALUE`, `SET_SCISSOR_HORIZONTAL`, `SET_ZSTENCIL_CLEAR_VALUE` |
| pass | `func_0045C338` | `func_0045DBB8` | 015 | 2 | `CLEAR_SURFACE`, `SET_COLOR_CLEAR_VALUE`, `SET_ZSTENCIL_CLEAR_VALUE` |
| pass | `func_004689A0` | `func_0046A220` | 016 | 9 | `SET_ALPHA_TEST_ENABLE`, `SET_CULL_FACE`, `SET_CULL_FACE_ENABLE`, `SET_DEPTH_TEST_ENABLE`, `SET_FRONT_FACE`, `SET_BLEND_ENABLE` e mais 4 |
| material | `func_0009FD98` | `func_0009FD98` | 002 | 0 | `SET_CULL_FACE_ENABLE` |
| material | `func_000A03DC` | `func_000A03DC` | 002 | 0 | `SET_TRANSFORM_CONSTANT_LOAD` |
| material | `func_000A1E18` | `func_000A1E18` | 002 | 1 | `SET_SHADER_PROGRAM` |
| material | `func_0045A3F4` | `func_0045BC74` | 015 | 0 | `SET_COLOR_MASK` |
| material | `func_0045A3F8` | `func_0045BC78` | 029 | 9 | `SET_COLOR_MASK` |
| material | `func_0046912C` | `func_0046A9AC` | 016 | 0 | `SET_ALPHA_FUNC`, `SET_BLEND_EQUATION`, `SET_BLEND_FUNC_SFACTOR`, `SET_STENCIL_FUNC`, `SET_STENCIL_OP_FAIL` |
| material | `func_00469130` | `func_0046A9B0` | 029 | 6 | `SET_ALPHA_FUNC`, `SET_BLEND_EQUATION`, `SET_BLEND_FUNC_SFACTOR`, `SET_STENCIL_FUNC`, `SET_STENCIL_OP_FAIL` |
| material | `func_00479050` | `func_0047A8D0` | 016 | 0 | `SET_TRANSFORM_CONSTANT_LOAD`, `SET_TRANSFORM_PROGRAM`, `SET_TRANSFORM_PROGRAM_LOAD`, `SET_VERTEX_ATTRIB_INPUT_MASK` |
| material | `func_0047A240` | `func_0047BAC0` | 016 | 0 | `SET_SHADER_CONTROL`, `SET_SHADER_PROGRAM`, `SET_VERTEX_ATTRIB_OUTPUT_MASK` |
| other | `func_0045B158` | `func_0045C9D8` | 015 | 1 | `INVALIDATE_L2` |
| other | `func_0046944C` | `func_0046ACCC` | 016 | 0 | `INVALIDATE_L2` |
| other | `func_00469450` | `func_0046ACD0` | 029 | 6 | `INVALIDATE_L2` |

## Medido em jogo (23/11)

Com a opção de build `PORT_DRAW_PROBE=ON` (desligada por padrão), as funções de desenho da edição
(`recompile.json`, `chunk_anchors` "draw") ganham um wrapper que conta cada chamada pelo endereço de retorno de
quem chamou (`port/core/draw_probe.cpp`); `PORT_DRAW_PROBE=<segundos>` imprime as chamadas por segundo. Rota com
`scripts/perf/drive_game.sh` e um save avançado, edição normal, GPU integrada:

| Fase | `func_00456548` (`DRAW_ARRAYS`) | `func_005B4718` (indexado) | `func_00456B38` |
|---|---|---|---|
| Logos e vídeo | 360/s | — | — |
| Menu principal e lista de missões | ~4.800/s | — | — |
| Luta (STG02) | ~9.250/s | ~2.160/s | nunca |

As constantes dos métodos de desenho (`0x1808`, `0x1814`, `0x1824`) só aparecem dentro dessas 3 funções: **todo
desenho do jogo passa por elas**. O RSX conta mais desenhos por quadro (340–800) porque cada chamada divide o
desenho em vários lotes de comando. Quem chama:

| Caminho | Normal | Premium | Chamadas |
|---|---|---|---|
| 2D: menus, telas e HUD | `func_0044F468` (chamada por `func_004502C8`) | `func_00450CE8` (`func_00451B48`) | 95% do `DRAW_ARRAYS` nos menus, ~150 por quadro na luta |
| Passes de tela cheia | `func_0045ACE8` (8 chamadores) | `func_0045C568` | 4 por quadro em todas as fases |
| Cena 3D | `func_005B2488` (chamada por `func_005B2930`, `func_005B3960`, `func_005B3EC0`) | `func_005B3D00` (`func_005B41A8`, `func_005B51D8`, `func_005B5738`) | ~36 por quadro na luta, cada uma em vários lotes |

`func_00456B38` não apareceu nas rotas medidas (menus, carregamento, luta).

## A lista de comandos do motor (23/11)

O caminho 2D não chama o RSX direto: `func_0044F468` (Premium `func_00450CE8`) é o **interpretador de comandos de
render do próprio motor**. Recebe em `r4` um registro cuja primeira palavra é a operação (0 a 52, despachada por
tabela de saltos em `0x0044F4B4`) e em `r3` o objeto de dispositivo de render; `func_004502C8` percorre a lista e
chama o interpretador para cada registro. Medido com `PORT_DRAW_PROBE` (o wrapper conta a operação de cada
registro): ~100 mil comandos por segundo nos menus e no HUD, sempre na mesma proporção.

| Operação | Por segundo | Handler | O que faz |
|---|---|---|---|
| 5 | ~58.500 | `func_00456AD0` | estado de render: alpha test, blend, cull, depth |
| 35 | ~17.600 | `func_00470E78` (cauda para `func_00470E7C`) | a decodificar |
| 9 | ~4.650 | `func_00456548` | **o desenho**: `DRAW_ARRAYS` (campos +4, +8, +C) |
| 20, 21, 23, 34 | ~4.500 cada | `func_004559E4`, `func_00455ABC`, `func_004559C0`, `func_00470FA0` | um de cada por desenho: gravam campos do dispositivo (`+0xCC`, `+0xD8`, `+0xDC`, `+0xE4`; a 23 zera `+0xCC`…`+0xF4`) |
| 32 | ~1.300 | `func_00470BE0` | a decodificar |
| 39, 41, 36, 37 | 120–780 | `func_00470B48` | a decodificar (estado de material) |
| 1 | 240 | `func_0045582C` | viewport (`SET_VIEWPORT_HORIZONTAL`, `SET_CLIP_MIN`) |
| 11, 12 | 0–240 | `func_0045A21C`, `func_0045A3F8` | blocos de estado (alpha, blend, cull, `INVALIDATE_L2`) |
| 25 | 60 | `func_0045ACE8` | passe de tela cheia |
| 42–45 | 60–120 | dentro do interpretador | blocos grandes sem chamadas, a decodificar |

As operações não falam com a GPU: gravam o estado num objeto de dispositivo (vários campos são ponteiros para
objetos do motor com tabela virtual), e só o desenho (op9) lê esse estado e emite os comandos RSX. **Um passe nativo
substitui o desenho em cima desse dispositivo**: ler os buffers, texturas, shaders e o estado que ele guarda e
desenhar em Vulkan, em vez de deixar `func_00456548` montar comandos do PS3.

## O dispositivo de render (23/11, em decodificação)

`PORT_DRAW_DUMP=<n>` (com `PORT_DRAW_PROBE`) registra, nas primeiras n chamadas de cada lugar que desenha, os
argumentos, os primeiros 0x200 bytes do dispositivo e o começo de cada objeto para que ele aponta. O desenho recebe
`r3` = dispositivo, `r4` = tipo de primitiva do motor, `r5` = primeiro vértice, `r6` = quantidade. Visto até aqui:

| Campo | Conteúdo |
|---|---|
| `+0x00`, `+0x08` | tabelas virtuais (a classe do dispositivo, uma interface) |
| `+0xB0`, `+0xB4` | largura e altura da tela (1280, 720) |
| `+0xC0` | objeto de superfície (endereço na memória do RSX, 1280×720): o alvo de render atual |
| `+0xCC` | declaração de vértices: objeto com a lista de elementos (tipo, uso, deslocamento) |
| `+0xD4` | flags do desenho (0 no passe de tela cheia, `0x101`/`0x109` nos sprites) |
| `+0xD8` | buffer de vértices fixo (endereço na memória do RSX, tamanho, contagem), no passe de tela cheia |
| `+0xDC` | endereço dos vértices deste desenho: memória do RSX no passe de tela cheia, um buffer circular na memória principal nos sprites |
| `+0x100`…`+0x10C` | o buffer de comandos RSX (início, fim, posição atual) |

Um sprite é desenhado como 6 vértices (dois triângulos) de **24 bytes**: posição em float3, cor RGBA8 e coordenada de
textura em float2. O passe de tela cheia usa 4 vértices de um buffer fixo. A textura não está nesses campos:
deve vir das operações 35/32 (a decodificar).

## Material: efeitos, texturas e samplers (23/11)

As operações 30–41 trabalham num **gerenciador de efeitos** (`*(TOC+0x3790)`), no estilo CgFX: o handle do comando
(`+0x8`) tem o efeito nos 16 bits altos (tabela em `gerenciador+0x5C`) e o parâmetro nos 16 baixos (tabela em
`efeito+0x70`). Os efeitos têm nome dentro do objeto (`efeito+0x4`). Com `PORT_DRAW_DUMP`, a sonda segue esses
handles e despeja efeito, parâmetro e, na op34, o objeto de textura.

| Operação | Registro | O que faz |
|---|---|---|
| 32 | efeito `0x20` | escolhe o efeito: `SH_PRETRANS_TEX_COL_P` no 2D (vértices já transformados, textura × cor) |
| 30 | efeito `0x1F` | vem antes da 32 (técnica ou passe; a confirmar) |
| 34 | parâmetro `0x8D`, valor = objeto de textura | liga uma textura ao sampler global `S_Simple` (efeito `0`, `GLOBAL`, `0x91` parâmetros) |
| 35 | parâmetro `0x8D`, índice 0/1/4/5, valor 3/3/2/2 | estado do sampler (wrap U/V, filtros) |
| 41 | ponteiro para dados, parâmetros `0x9`, `0xA`, `0xB`, `0x61` | constantes lidas da memória (matrizes ou vetores) |

O **objeto de textura** (classe com tabela virtual `006BD440`, a mesma do alvo de render em `dispositivo+0xC0`) tem
em `+0x04` o endereço na memória do RSX e em `+0x48` um ponteiro para um `CellGcmTexture` completo. O da tela de
logos: formato `0x88` (DXT5), 1 mip, 2D, remap `0xAAE4`, 2048×1024, pitch `0x2000`, offset `0x03214000`.

Com isso, um desenho 2D tem tudo o que precisa para ser feito em Vulkan sem comandos RSX: o efeito (shader trivial),
a textura (o `CellGcmTexture`, que o backend já sabe ler), o sampler, os vértices (24 bytes, no `dispositivo+0xDC`),
o alvo (`dispositivo+0xC0`) e o estado de render da op5 (campos do dispositivo ainda a mapear).

## Passe nativo do 2D, etapa 1: captura (23/11)

Opção de build `PORT_NATIVE_PASS` (desligada por padrão; `port/graphics/native_pass.cpp`). Os mesmos wrappers da
sonda passam os comandos do motor e os desenhos do caminho 2D para um rastreador de estado: a op32 dá o efeito, a op34
a textura (o `CellGcmTexture` do objeto), a op35 o sampler, e no desenho os vértices são copiados do buffer circular.
O estado de render não precisa ser rastreado: a op5 emite blend, depth e cull direto no buffer de comandos RSX
(`func_004689A0`), onde o motor RSX os vê. `PORT_NATIVE_PASS=capture[:<segundos>]` imprime o que foi capturado; nada
do que o jogo desenha muda.

| | Logos e título | Menus | Luta |
|---|---|---|---|
| Desenhos do caminho 2D por segundo | ~1.480 | ~4.700 | ~4.050 |
| Capturados com `SH_PRETRANS_TEX_COL_P` | ~1.145 | ~3.940 | ~3.275 |

- Efeitos no caminho 2D: `SH_PRETRANS_TEX_COL_P` (a maioria), `SH_PRETRANS_TEX_COL_MUL_P`, `SH_PRETRANS_COL_P` (sem
  textura) e, nos menus, `SH_BLOOMFAKE_EXTRACT_FILTER_P`, `SH_BLOOMFAKE_DOWN_FILTER_P` e `SH_BLOOMFAKE_FILTER_P`.
- Texturas: só DXT1 (`0x86`), DXT3 (`0x87`) e DXT5 (`0x88`), que o Vulkan desenha direto (BC1/BC2/BC3).
- Primitivas do motor: 2 (lista de triângulos, 6 vértices por sprite) e 4 (4 vértices, quadros de tela cheia).
- Exemplo: sprite de 6 vértices, textura DXT5 512×256 em `0x033F4000`, primeiro vértice (-0,400, 0,356, 1,0), cor
  branca, UV (0, 0).

## Passe nativo do 2D, etapa 3: substituição (23/11)

`PORT_NATIVE_PASS=hud` (no build com a opção `PORT_NATIVE_PASS`). Um desenho do caminho 2D com o efeito
`SH_PRETRANS_TEX_COL_P`, textura DXT1/3/5 e a declaração de vértices dos sprites não roda como código do jogo:

1. **PPU** (`native_pass.cpp`): o registro (triângulos já no layout do host, textura, sampler) vai para uma fila; o
   wrapper roda o flush de estado do próprio jogo (`func_00455E08`, âncora `draw_flush`: é ele que liga o alvo de
   render, de forma preguiçosa) e grava no buffer de comandos, no lugar do desenho, um `NO_OPERATION` com
   `0xBA7E0000 | id`. Sem espaço no segmento, o desenho do jogo roda.
2. **Motor RSX** (`rsx_engine_bridge.c`, `eng_native_draw`): ao chegar no marcador, desenha o registro com o alvo, o
   estado de render, o viewport e o scissor daquele ponto da sequência; shader próprio (vértice: só os atributos
   0, 3 e 8, para casar com o layout compacto; fragmento: `textura × saturate(cor)` e o alpha test do estado RSX); a
   textura pelo cache do motor.

Resultado, GPU integrada: título, menu principal, tela de configuração da missão e o HUD da luta (retrato, barra de
vida, minimapa, estilo, painel de tutorial) **iguais** aos do motor RSX. ~3.700 desenhos nativos por segundo no
título, ~8.800 nos menus, ~7.300 na luta; o resto (tela cheia, outros efeitos e formatos) continua com o jogo.
**Premium** (BLJS10219): o mesmo passe, com as âncoras e os deslocamentos de TOC da edição (`recompile.json`:
`toc_effect_manager` `0x3818`, `toc_primitive_table` `0x3888`, `0x88` adiante da normal). O título da Premium (com o
selo "PREMIUM TV SOUND EDITION") e o menu saem iguais ao motor RSX, ~3.700 desenhos nativos por segundo.

Custo, STG02 60 s, GPU integrada, máquina livre: sem os ganchos 51,4 fps (busy 18,48 ms); com os ganchos e o passe
desligado 51,3 (18,56); **com o passe nativo 52,0 (18,26)**, p95 23,2 ms nos três: o PPU deixa de montar os comandos
RSX dos sprites. Diagnóstico: `PORT_NATIVE_PASS=hud-both` (o nativo e o do jogo, um sobre o outro) e `PORT_NATIVE_DEBUG=1|2|3`
(magenta, só textura, só cor). O que custou achar: o vertex shader embutido do motor declara os 16 atributos, e com
o layout compacto a cor e a UV caíam nas entradas erradas; e sem o flush do jogo o alvo de render nunca era ligado.

## Passe nativo do 2D: mais efeitos e tela cheia (23/11)

`PORT_NATIVE_PASS=hud` registra a cada 5 s por que cada desenho ficou com o jogo (efeito, declaração, contagem).
Na rota logos → título → menus → luta, sobravam `SH_PRETRANS_COL_P`, `SH_PRETRANS_TEX_COL_MUL_P` e
`SH_PRETRANS_TEX_P`, e os passes de tela cheia.

**Os programas de cada efeito.** O flush de estado do jogo liga o programa de fragmento antes do marcador, então o
motor sabe qual é, e `RSX_ENGINE_SHADER_DUMP` dá a tradução:

| Efeito | Programa (`location 0`) | O que faz |
|---|---|---|
| `SH_PRETRANS_TEX_COL_P` | `0x01223480` | `textura × saturate(cor)` |
| `SH_PRETRANS_COL_P` | `0x012235C0` | `saturate(cor)` |
| `SH_PRETRANS_TEX_COL_MUL_P` | `0x01223540` | `rgb = tex.a·(1−tex.rgb)·cor.rgb·cor.a`, `a = tex.a·cor.a`; a textura no alvo 1 |
| `SH_PRETRANS_TEX_P` | `0x01221000` | `textura` (o vídeo de abertura) |

Os quatro estão em `rsx_engine_bridge.c` (`kEngNativeColour`), com o alpha test do estado RSX. O registro leva o tipo
(`PORT_NATIVE_TEX_COL` …) e o motor guarda um pipeline por tipo e estado.

**Textura pela unidade 0.** O registro levava a textura da op34 no sampler global `S_Simple`; o `SH_PRETRANS_TEX_P`
usa outro parâmetro, e o vídeo saía com uma textura velha. Na hora do marcador, a unidade 0 do RSX já tem a textura e
o sampler certos (o flush do jogo os ligou; batem com o registro em ~95% dos casos, e os outros eram o vídeo). O
motor agora usa o próprio bind (`sink_bind_textures`), que trata alvos de render, profundidade, cópias de feedback e
qualquer formato. Os campos de textura e sampler saíram do registro.

**Tela cheia.** Declaração de 20 bytes `{1,3,0,0, 8,2,0xC,0}`: posição float3 e coordenada float2, num buffer fixo na
memória do RSX, sem cor. A cor é o valor atual do atributo 3 com a regra do caminho de arrays do motor (nunca escrito
vale branco). O quadro de cor de tela cheia (`SH_PRETRANS_COL_P` sem textura) sai branco e opaco por cima dos logos
da abertura, o que o desenho do jogo não faz, com o mesmo estado (blend desligado, sem profundidade nem stencil,
mesmo alvo); o atributo 3 aponta para a memória principal com tamanho 0 e stride 0. Até entender, esse caso fica com
o jogo.

Resultado, GPU integrada, edição normal e Premium: logos, vídeo, título, menus, configuração da missão, cena de
carregamento e luta iguais ao motor RSX. Na luta, ~7.500 desenhos nativos por segundo e ~360 com o jogo (os filtros
`SH_BLOOMFAKE_*` e `SH_MATERIAL_SHADOW_P`); na cena com profundidade de campo, também DOF, FXAA, gauss e radial
blur. STG02 60 s: 48,9 fps com o passe desligado, **49,7** com ele (máquina mais lenta que na medição anterior).
Diagnóstico: `PORT_NATIVE_EFFECTS=<lista>` limita os efeitos nativos; `PORT_NATIVE_DEBUG` registra o programa de
cada efeito e, nos passes de tela cheia, os arrays de vértice e o estado.

## Passe nativo: o shader pelo programa ligado, e os filtros de tela cheia (23/11)

**O nome do efeito não serve nos passes de tela cheia.** `PORT_NATIVE_PROBE=<n>` grava, depois de um desenho que o
jogo fez, um `NO_OPERATION` `0xBA7D0000 | efeito`, e o motor descreve o que o desenho usou (programa, unidades de
textura, constantes, alvo, arrays de vértice). Mostrou que a op32 fica velha nos passes de tela cheia: o "quadro de
cor" branco era na verdade a **cópia de textura** (programa `0x01221000`) de um alvo de render, e o motor desenhava
cor pura porque confiava no nome.

**O motor escolhe pelo programa.** No marcador, o motor pega o par de programas que o flush do jogo ligou
(`eng_pipeline_get`: tradução em cache, hash estrutural do programa de fragmento, unidades amostradas, constantes) e:

- se o hash está em `kEngNativePrograms`, desenha com o shader escrito à mão para aquele programa, com as constantes e
  texturas do jogo (`C[i]`, `S(unidade, uv)`, `L(uv, lod)`);
- se não, desenha o mesmo registro com a tradução do programa do jogo, com o layout de vértices que o vertex program
  dele lê (`eng_vertex_layout`; o layout fixo de três atributos punha a UV no lugar da cor). Isso é a rede de
  segurança; com `PORT_NATIVE_DEBUG` cada programa novo que cai nela é registrado.

O PPU não filtra mais pelo efeito: lê a declaração de vértices de forma genérica (posição float3, cor de 4 bytes tipo
9 ou 5, coordenada float2) e aceita lista, *strip* e leque.

**Programas com shader nativo** (os hashes são iguais nas duas edições):

| Programa | O que faz |
|---|---|
| `b232746bc7864749` | sprite: textura × cor |
| `b9a97ce6812306eb` | sprite: cor |
| `0a9d5f2af539481f` | sprite: cor escurecida pela textura, textura no alvo 1 |
| `0a5f2826bc7492d5` | cópia de textura (composições, vídeo) |
| `4aee957cd2006214` | bloom: realce (limiar `C0.x`) |
| `eb9e84f065412dac` | bloom: redução de 4 amostras |
| `38d60a2ecfc7e46f` | bloom: composição com a cena |
| `fc192b89a2eade06` | profundidade de campo: fator de desfoque pela profundidade |
| `9e5442311c2e6b9f` | profundidade de campo: redução de 4 amostras |
| `95e5e66b86551cae` | profundidade de campo: mistura nítida/desfocada |
| `e721c77062f5c3b6` | gauss 5×5 (13 amostras, deslocamento e peso nas constantes) |
| `a2f8fe6ed13c85ad` | radial blur (até 16 amostras em direção ao centro) |
| `cb6f65c49800f61d` | FXAA 3.11, variante de console |

Resultado, GPU integrada, edição normal e Premium: logos, vídeo, título, menus, configuração da missão, cutscene com
profundidade de campo e luta iguais ao motor RSX, e **nenhum desenho do caminho 2D com o jogo** (antes ~360/s na luta,
~900/s nas cenas); nenhum programa caiu na tradução nessa rota. `PORT_NATIVE_PROGRAMS=none` (tudo pela tradução)
também sai igual, o que valida a rede de segurança. STG02 60 s: 52,7 fps com o passe e 53,1 sem, numa segunda
medição (a primeira deu 57,4 contra 51,8; a variação entre execuções é dessa ordem): custo neutro. O radial blur ainda
não foi comparado quadro a quadro (aparece pouco na rota).

## Próximos passos

1. Pegar também os métodos calculados (texturas, constantes de vértice) e os comandos da `libgcm`, para o mapa de
   material ficar completo.
2. Mapear os campos de estado de render que a op5 grava no dispositivo (blend, alpha, depth, cull).
3. Cobrir mais telas (galeria, loja, briefing, outras fases) com `PORT_NATIVE_DEBUG`, escrevendo à mão os programas
   que caírem na tradução.
4. Ligar a opção `PORT_NATIVE_PASS` no build por padrão, com o passe ainda escolhido em tempo de execução, depois de
   cobrir mais telas (galeria, loja, briefing) e o HUD de mais fases.
5. Depois do 2D, a cena 3D (`func_005B2488`): o mesmo dispositivo, com outros efeitos e buffers indexados.
