# Sensor de cores feito com um resistor LDR

## Objetivo

Desenvolver um sistema embarcado baseado no microcontrolador RP2040 capaz de identificar a cor de brinquedos em uma esteira de produção por meio de um sensor LDR, acionando um servomotor acoplado a um braço deflector para direcionar ou descartar automaticamente cada peça conforme sua cor, garantindo o controle de qualidade na linha de produção.

## Descrição Geral do Sistema

O sistema utiliza três LEDs (vermelho, verde e azul) que iluminam o objeto sobre a esteira sequencialmente. Para cada cor de LED acesa, um LDR mede a tensão refletida pelo objeto via ADC do RP2040 — formando uma assinatura RGB em voltagem para cada peça.

Antes de entrar em operação, o sistema passa por duas etapas de calibração via interface serial (UART/USB): primeiro, a calibração do tempo de decaimento do LDR (para garantir que o canal anterior apagou completamente antes de acender o próximo); depois, o treinamento de até 7 perfis de cor, incluindo obrigatoriamente o perfil NONE — a assinatura da esteira vazia, usada como filtro de ruído.

Durante a operação contínua, a cada leitura o firmware calcula a distância euclidiana no espaço RGB entre a leitura atual e todos os perfis treinados. Um filtro de limiar rejeita reflexos fracos da própria esteira. Se uma peça válida for identificada, o servomotor é travado no ângulo configurado para aquela cor por 2,8 segundos — tempo suficiente para a peça passar pelo braço deflector e ser direcionada ou descartada. Um LED RGB de status pisca em branco enquanto o sistema aguarda peças.

Toda a configuração e monitoramento é feita via comunicação serial (UART/USB), sem display físico.

## Diagrama de Blocos


```mermaid
flowchart LR
    C1[Inicializa o sistema] --> C2[Mede latência\ndos LEDs]
    C2 --> C3{Calibrar cor\nou iniciar sensor?}
    C3 -- Calibrar --> C4[Usuário\nescolhe a cor]
    C4 --> C5[Coloca objeto\nno sensor]
    C5 --> C6[LEDs piscam\nsequencialmente]
    C6 --> C7[LDR registra\nresistência por canal]
    C7 --> C8[Divisor de tensão\nconverte em tensão]
    C8 --> C9[ADC do RP2040\nlê os valores]
    C9 --> C10[RP2040 armazena\nassinatura RGB]
    C10 --> C11[Usuário define\nângulo do servo]
    C11 --> C3
    C3 -- Iniciar sensor --> OP1[LEDs piscam\nsequencialmente]
    OP1 --> OP2[LDR registra\nresistência R, G, B]
    OP2 --> OP3[Divisor de tensão\nconverte em tensão]
    OP3 --> OP4[ADC do RP2040\nlê os valores]
    OP4 --> OP5[Firmware compara com\ncalibrações armazenadas]
    OP5 -- Cor reconhecida --> OP6[Servo aciona no\nângulo calibrado]
    OP5 -- Esteira vazia\nou ruído --> OP7[Aguarda\nLED pisca branco]
    OP6 --> OP8{Cor correta?}
    OP8 -- Sim --> OP9[Braço para a direita\nPeça segue produção]
    OP8 -- Não --> OP10[Braço para a esquerda\nPeça descartada]
```

## Hardware

### Esquema Elétrico do sensor de cor LDR
![Esquemático](images/ESQUEMATICA.png
)

### PCB do Sensor

#### Top Layer

![PCB Top Layer do controlador](images/TopLayer.png)

#### Bottom Layer

![PCB Bottom Layer do controlador](images/BottomLayer.png)


## Funcionalidades

- Leitura analógica de cor via sensor LDR
- Condicionamento de sinal (amplificação/filtragem)
- Classificação da cor identificada via firmware no RP2040
- Acionamento automático do motor da esteira/atuador

## Projeto Finalizado
![Projeto Finalizado](images/FotoProjeto.png)
## Estrutura do Repositório

```
CoresLDR_Esteira/
├── README.md
├── LICENSE    
├── Codigo/          # Código-fonte em C (SDK RP2040)
├── docs/                # Relatório técnico e apresentação
├── GerbersPCB/           # Arquivos relacionados a fabricação da PCB
├── images/              # Fotos do projeto e PCB
└── Solid_Parts/         # Arquivos da estrutura mecânica (SLDPRT) 

```

## Documentação

- 📄 **Relatório técnico:** [docs/relatorio-tecnico.pdf](docs/relatorio-tecnico.pdf)
- 🎤 **Apresentação:** [docs/apresentacao.pptx](docs/apresentacao.pptx)


## Integrantes

| Nome | RA |
|------|-----|
| Tiago Tosto Pereira Regente | 23.00815-6 |
| Felipe Cerquiaro da Silva Trancho | 22.01106-4 |
| Pedro Frehse Baltar | 23.95013-7 |
| Rafael Panicali Mello Guida | 21.00423-4 |

## Disciplina

Projeto Integrado das Disciplinas Instrumentação e Microcontroladores e Sistemas Microcontrolados — Instituto Mauá de Tecnologia
Prof. Andressa Martins e Prof. Rodrigo França
