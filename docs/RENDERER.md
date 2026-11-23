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

## Próximos passos

1. Pegar também os métodos calculados (texturas, constantes de vértice) e os comandos da `libgcm`, para o mapa de
   material ficar completo.
2. Mapear os campos de estado de render que a op5 grava no dispositivo (blend, alpha, depth, cull).
3. Protótipo do primeiro passe nativo (`PORT_NATIVE_PASS=hud`): no desenho do caminho 2D, montar o desenho Vulkan a
   partir do dispositivo e do gerenciador de efeitos, só para `SH_PRETRANS_TEX_COL_P`, e comparar a imagem com o motor
   RSX desenhando a mesma coisa.
4. Depois do 2D, a cena 3D (`func_005B2488`): o mesmo dispositivo, com outros efeitos e buffers indexados.
