# **Documentação Oficial Linguagem Luegiu PT-BR**

## **SINTAXE**

### Inteiros

- int8
- int16
- int32
- int64
- char (mais explicações abaixo)
- uint8
- uint16
- uint32
- uint64
- big
- small
- hexa

Char é utilizado como inteiro ou texto, no caso de inteiro ele é convertido para bytes e consequentente podemos usar caracteres para somas

Decisões de design da linguagem
Separamos int para ter controle total int convencial das linguagem por favor usar int32
big é um int64 e para transformar em unsigned use up
small é um int8 e para transformar em unsigned use up
hexa é um inteiro mas não é possivel usar ele com outros numeros, mas é possivel usar ele para o link (explicado na parte de Especial)

### Textos

- char
- utf16char

### Decimais

- float
- double

### Especial

- link
- void

Link use como void* para manter a sintaxe limpar, recomendaremos fortemente a utilização dele por motivos de otimização da linguagem

### Caso queira deixar explicito o unsigned use up


