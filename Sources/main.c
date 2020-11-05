#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "binaryen-c.h"

typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define MemorySet               memset
#define MemoryCopy              memcpy
#define CalculateCStringLength  strlen
#define CStringToInt            atoi
#define QuickSort               qsort
#define Log(...) { fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); }

static int
CharIsAlpha(int c)
{
    return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}

static int
CharIsDigit(int c)
{
    return (c >= '0' && c <= '9');
}

static int
CharIsSpace(int c)
{
    return (c <= 32);
}

static char symbols[6] = {'=','{','}','*','|','`'};
static int
CharIsSymbol(int c)
{
	for(int i = 0; i < CalculateCStringLength(symbols);i++){
		if(symbols[i] == c){
			return 1;
		}
	}
    return 0;
}

static int
CharIsText(int c)
{
    return (!CharIsSymbol(c) && c != '@');
}

static int
CStringMatchCaseSensitiveN(char *a, char *b, int n)
{
    int matches = 0;
    if(a && b && n)
    {
        matches = 1;
        for(int i = 0; i < n; ++i)
        {
            if(a[i] != b[i])
            {
                matches = 0;
                break;
            }
        }
    }
    return matches;
}

static int
CStringMatchCaseInsensitive(char *a, char *b)
{
    int matches = 0;
    if(a && b)
    {
        matches = 1;
        for(int i = 0;; ++i)
        {
            if(a[i] != b[i])
            {
                matches = 0;
                break;
            }
            else if(!a[i])
            {
                break;
            }
        }
    }
    return matches;
}

typedef u32 OutputFlags;
#define OutputFlag_WASM      (1<<0)
#define OutputFlag_C         (1<<1)
#define OutputFlag_js    (1<<2)

typedef enum InputType
{
	InputType_Invalid,
    InputType_OR,
    InputType_WASM,
}
InputType;


typedef enum ExprType
{
    ExprType_Invalid,
    ExprType_Var,
    ExprType_Const,
    ExprType_Func,
}
ExprType ;

typedef enum TokenType
{
    Token_None,
	Token_Var,
	Token_Const,
    Token_Func,
    Token_Int,
	Token_Float,
    Token_DoubleNewline,
    Token_Symbol,
    Token_StringConstant,
}
TokenType;

typedef struct Token Token;
struct Token
{
    TokenType type;
	Token* tokens;
    char *string;
    int string_length;
    int lines_traversed;
};

typedef struct ExprNode ExprNode;
struct ExprNode
{
    ExprType type;
	Token* tokens;
    int tokens_length;
    ExprNode *next;
    ExprNode *first_parameter;
    
    union
    {
        struct
        {
            ExprNode *first_item;
        }
        unordered_list;
        
        struct
        {
            ExprNode *first_item;
        }
        ordered_list;
    };
};

typedef struct Tokenizer Tokenizer;
struct Tokenizer
{
    char *at;
    int line;
    char *file;
    int break_text_by_commas;
};

#define PARSE_CONTEXT_MEMORY_BLOCK_SIZE_DEFAULT 4096
typedef struct ParseContextMemoryBlock ParseContextMemoryBlock;
struct ParseContextMemoryBlock
{
    int size;
    int alloc_position;
    void *memory;
    ParseContextMemoryBlock *next;
};

typedef struct ParseError ParseError;
struct ParseError
{
    char *file;
    int line;
    char *message;
};

typedef struct ParseContext ParseContext;
struct ParseContext
{
    ParseContextMemoryBlock *head;
    ParseContextMemoryBlock *active;
    int error_stack_size;
    int error_stack_size_max;
    ParseError *error_stack;
};

static void *
ParseContextAllocateMemory(ParseContext *context, int size)
{
    void *memory = 0;
    
    ParseContextMemoryBlock *chunk = context->active;
    if(!chunk || chunk->alloc_position + size > chunk->size)
    {
        ParseContextMemoryBlock *old_chunk = chunk;
        int needed_bytes = size < PARSE_CONTEXT_MEMORY_BLOCK_SIZE_DEFAULT ? PARSE_CONTEXT_MEMORY_BLOCK_SIZE_DEFAULT : size;
        chunk = malloc(sizeof(ParseContextMemoryBlock) + needed_bytes);
        chunk->memory = (char *)chunk + sizeof(ParseContextMemoryBlock);
        chunk->size = needed_bytes;
        chunk->alloc_position = 0;
        chunk->next = 0;
        if(old_chunk)
        {
            old_chunk->next = chunk;
        }
        else
        {
            context->head = chunk;
        }
        context->active = chunk;
    }
    
    memory = (char *)chunk->memory + chunk->alloc_position;
    chunk->alloc_position += size;
    return memory;
}

static char *
ParseContextAllocateCStringCopy(ParseContext *context, char *str)
{
    int needed_bytes = CalculateCStringLength(str)+1;
    char *str_copy = ParseContextAllocateMemory(context, needed_bytes);
    MemoryCopy(str_copy, str, needed_bytes);
    return str_copy;
}

static char *
ParseContextAllocateCStringCopyN(ParseContext *context, char *str, int n)
{
    int needed_bytes = n+1;
    char *str_copy = ParseContextAllocateMemory(context, needed_bytes);
    MemoryCopy(str_copy, str, needed_bytes);
    str_copy[n] = 0;
    return str_copy;
}

static ExprNode *
ParseContextAllocateNode(ParseContext *context)
{
    ExprNode *node = ParseContextAllocateMemory(context, sizeof(ExprNode));
    MemorySet(node, 0, sizeof(*node));
    return node;
}

static Token*
ParseContextAllocateToken(ParseContext *context)
{
    Token *t = ParseContextAllocateMemory(context, sizeof(Token));
    MemorySet(t, 0, sizeof(*t));
    return t;
}

static void
PushParseError(ParseContext *context, Tokenizer *tokenizer, char *format, ...)
{
    if(!context->error_stack)
    {
        context->error_stack_size = 0;
        context->error_stack_size_max = 32;
        context->error_stack = ParseContextAllocateMemory(context, sizeof(ParseError)*context->error_stack_size_max);
    }
    
    if(context->error_stack_size < context->error_stack_size_max)
    {
        va_list args;
        va_start(args, format);
        int needed_bytes = vsnprintf(0, 0, format, args)+1;
        va_end(args);
        
        char *message = ParseContextAllocateMemory(context, needed_bytes);
        
        va_start(args, format);
        vsnprintf(message, needed_bytes, format, args);
        va_end(args);
        
        message[needed_bytes-1] = 0;
        
        ParseError error = {0};
        {
            error.file = tokenizer->file;
            error.line = tokenizer->line;
            error.message = message;
        }
        
        context->error_stack[context->error_stack_size++] = error;
    }
}




static Token
GetNextTokenFromBuffer(Tokenizer *tokenizer)
{
    char *buffer = tokenizer->at;
    Token token = {0};
    
    for(int i = 0; buffer[i]; ++i)
    {
        // NOTE(jsn): Newline
        if(buffer[i] == '\n' && buffer[i+1] == '\n')
        {
            token.type = Token_DoubleNewline;
            token.string = buffer+i;
            token.string_length = 2;
            break;
        }
		else if(buffer[i] == 'v' && buffer[i+1] == 'a' && buffer[i+2] == 'r'){
			token.type = Token_Var;
            token.string = buffer+i;
            token.string_length = 0;
			int escaped = 1;
			for(; (token.string[token.string_length] != '=' && token.string[token.string_length] != ':' || escaped) && token.string[token.string_length];
                ++token.string_length)
            {
                if(escaped)
                {
                    escaped = 0;
                }
                else
                {
                    if(token.string[token.string_length] == '\\')
                    {
                        escaped = 1;
                    }
                }
            }
            break;
		}
		else if(buffer[i] == 'c' && buffer[i+1] == 'o' && buffer[i+2] == 'n' && buffer[i+3] == 's' && buffer[i+4] == 't'){
			token.type = Token_Const;
            token.string = buffer+i;
            token.string_length = 0;
			int escaped = 1;
			for(; (token.string[token.string_length] != ';' || escaped) && token.string[token.string_length];
                ++token.string_length)
            {
                if(escaped)
                {
                    escaped = 0;
                }
                else
                {
                    if(token.string[token.string_length] == '\\')
                    {
                        escaped = 1;
                    }
                }
            }
            break;
		}
        // NOTE(jsn): String Constant
        else if(buffer[i] == '"')
        {
            token.type = Token_StringConstant;
            token.string = buffer+i;
            token.string_length = 0;
            int escaped = 1;
            for(; (token.string[token.string_length] != '"' || escaped) && token.string[token.string_length];
                ++token.string_length)
            {
                if(escaped)
                {
                    escaped = 0;
                }
                else
                {
                    if(token.string[token.string_length] == '\\')
                    {
                        escaped = 1;
                    }
                }
            }
            
            ++token.string_length;
            
            break;
        }
        // NOTE(jsn): Int
        // @TODO: Fix me we need to consider only numbers and only returnn a string of numbers
        else if(CharIsDigit(buffer[i])){
            token.type = Token_Int;
            token.string = buffer+i;
            token.string_length = 0;
            int strlen = 0;
            for(; token.string[strlen] != ';' && token.string[strlen];
                ++strlen)
            {
                int isValid = CharIsDigit(token.string[strlen]) || CharIsSpace(token.string[strlen]);
                if(!isValid)
                {
                    break;//We will push an error since we have an invalid value
                }
                else{
                    ++token.string_length;
                }
            }
            // if(token.string[token.string_length] == ';')
            //     --token.string_length;
            break;
        }
        //@TODO: Use this for functions eventually
        else if(!CharIsSpace(buffer[i]))
        {
            int j = 0;
            
            // NOTE(rjf): Symbol
            if(CharIsSymbol(buffer[i]))
            {
                static char *symbolic_blocks_to_break_out[] =
                {
                    "=",
                    "*",
                    "_",
                    "`",
                    "{",
                    "}",
                    "|",
                    ";",
                };
                
                for(j=i+1; buffer[j] && CharIsSymbol(buffer[j]); ++j);
                token.type = Token_Symbol;
                
                for(int k = 0; k < sizeof(symbolic_blocks_to_break_out)/sizeof(symbolic_blocks_to_break_out[0]);
                    ++k)
                {
                    int length_to_compare = CalculateCStringLength(symbolic_blocks_to_break_out[k]);
                    if(CStringMatchCaseSensitiveN(symbolic_blocks_to_break_out[k], buffer+i, length_to_compare))
                    {
                        j = i+length_to_compare;
                        break;
                    }
                }
            }
            
        //     // NOTE(rjf): Text
        //     else
        //     {
        //         for(j=i+1;
        //             buffer[j] &&
        //             CharIsText(buffer[j]) &&
        //             buffer[j] != '\n' &&
        //             (!tokenizer->break_text_by_commas ||
        //              buffer[j] != ','); ++j);
        //         token.type = Token_Text;
                
        //         // NOTE(rjf): Add skipped whitespace to text node
        //         for(; i > 0 && CharIsSpace(buffer[i-1]); --i);
        //     }
            
            if(j != 0)
            {
                token.string = buffer+i;
                token.string_length = j-i;
                break;
            }
        }
    }
	
    
    for(int i = 0; i < token.string_length; ++i)
    {
        if(token.string[i] == '\n')
        {
            ++token.lines_traversed;
        }
    }
    
    return token;
}

static Token
PeekToken(Tokenizer *tokenizer)
{
    Token token = GetNextTokenFromBuffer(tokenizer);
    return token;
}

static Token
NextToken(Tokenizer *tokenizer)
{
    Token token = GetNextTokenFromBuffer(tokenizer);
    tokenizer->at = token.string + token.string_length;
    tokenizer->line += token.lines_traversed;
    return token;
}

static int
RequireTokenType(Tokenizer *tokenizer, TokenType type, Token *token_ptr)
{
    int match = 0;
    Token token = GetNextTokenFromBuffer(tokenizer);
    if(token.type == type)
    {
        match = 1;
        if(token_ptr)
        {
            *token_ptr = token;
        }
        tokenizer->at = token.string + token.string_length;
        tokenizer->line += token.lines_traversed;
    }
    return match;
}

static int
TokenMatch(Token token, char *string)
{
    return (token.type != Token_None &&
            CStringMatchCaseSensitiveN(token.string, string, token.string_length) &&
            string[token.string_length] == 0);
}

static int
RequireToken(Tokenizer *tokenizer, char *string, Token *token_ptr)
{
    int match = 0;
    Token token = GetNextTokenFromBuffer(tokenizer);
    if(TokenMatch(token, string))
    {
        match = 1;
        if(token_ptr)
        {
            *token_ptr = token;
        }
        tokenizer->at = token.string + token.string_length;
        tokenizer->line += token.lines_traversed;
    }
    return match;
}

static void
SkipToAfterNextComma(Tokenizer *tokenizer)
{
    for(int i = 0; tokenizer->at[i]; ++i)
    {
        if(tokenizer->at[i] == ',')
        {
            tokenizer->at += i+1;
            break;
        }
    }
}

static void
TrimQuotationMarks(char **text, int *text_length)
{
    if(*text[0] == '"')
    {
        *text = *text + 1;
        *text_length -= 2;
    }
}

static i8 
GetValue(ParseContext *context, Tokenizer *tokenizer, char* link,Token* value){
    int link_length = 0;
    i8 isSet = 0;
    int bracket_stack = 0;
    for(int i = 0; link[i]; ++i)
    {
        if(link[i] == '"')
        {
            if(RequireTokenType(tokenizer, Token_StringConstant, value)){
                i += value->string_length;
                isSet = 1;
            }
            else{
                PushParseError(context, tokenizer, "Expected \" to follow String assignation.");
            }
            break;
        }
        else if(link[i] == '[')
        {
            bracket_stack++;
        }
        else if(link[i] == ']')
        {
            --bracket_stack;
        }
        else if(CharIsDigit(link[i]) && RequireTokenType(tokenizer, Token_Int, value))
        {
            if(CharIsDigit(value->string[value->string_length-1]))
            {
                isSet = 1;
            }
            else {
                PushParseError(context, tokenizer, "Expected Int value but had incorrect char %c.",value->string[value->string_length-1]);
            }
            i += value->string_length;
        }
        
        if(link[i] == ';')
        {
            ++link_length;
            break;
        }
        
        ++link_length;
    }
    if(isSet == 0){
        PushParseError(context, tokenizer, "Expected value before endline");
    }
    return isSet == 0 ? isSet : link_length;
}
static void
setTokenFromOther(Token* to, Token from){
    to->type = from.type;
    to->string = from.string;
    to->string_length = from.string_length;
    to->lines_traversed = from.lines_traversed;
}
static ExprNode *
ParseText(ParseContext *context, Tokenizer *tokenizer)
{
    ExprNode *result = 0;
    
    Token token = PeekToken(tokenizer);
    int text_style_flags = 0;
    
    ExprNode **node_store_target = &result;
    
    while(token.type != Token_None)
    {
        Token isVar = {0};
        Token symbol = {0};
        Token text = {0};
        
        if(RequireTokenType(tokenizer, Token_Var, &isVar))
        {
            Token* var = ParseContextAllocateToken(context);
            setTokenFromOther(var,isVar);
			Token isAssign = {0};
            tokenizer->at = var->string + var->string_length;
			if(RequireToken(tokenizer, "=", &isAssign))
			{
                Token* assign = ParseContextAllocateToken(context);
                setTokenFromOther(assign,isAssign);
				char *link = assign->string+1;
				Token* value = ParseContextAllocateToken(context);
                int link_length = GetValue(context,tokenizer,link,value);
				if(link_length){
					ExprNode *node = ParseContextAllocateNode(context);
					assign->tokens = value;
					var->tokens = assign;
					node->tokens = var;
                    node->tokens_length = 3;
					node->type = ExprType_Var;
					*node_store_target = node;
					node_store_target = &(*node_store_target)->next;
					
					tokenizer->at = link + link_length;
                }
				

			}
            // else if(TokenMatch(tag, "@Code"))
            // {
            //     Token open_bracket = {0};
            //     if(RequireToken(tokenizer, "{", &open_bracket))
            //     {
            //         char *link = open_bracket.string+1;
            //         int link_length = 0;
                    
            //         int bracket_stack = 1;
            //         for(int i = 0; link[i]; ++i)
            //         {
            //             if(link[i] == '{')
            //             {
            //                 ++bracket_stack;
            //             }
            //             else if(link[i] == '}')
            //             {
            //                 --bracket_stack;
            //             }
                        
            //             if(bracket_stack == 0)
            //             {
            //                 break;
            //             }
                        
            //             ++link_length;
            //         }
                    
            //         ExprNode *node = ParseContextAllocateNode(context);
            //         node->type = ExprType_Func;
            //         node->string = link;
            //         node->string_length = link_length;
            //         *node_store_target = node;
            //         node_store_target = &(*node_store_target)->next;
                    
            //         tokenizer->at = link + link_length;
            //         if(!RequireToken(tokenizer, "}", 0))
            //         {
            //             PushParseError(context, tokenizer, "Expected } to follow code block.");
            //         }
            //     }
            //     else
            //     {
            //         PushParseError(context, tokenizer, "A code tag expects {<code>} to follow.");
            //     }
            // }
            // else if(TokenMatch(tag, "@Lister"))
            // {
            //     Token open_bracket = {0};
            //     if(RequireToken(tokenizer, "{", &open_bracket))
            //     {
                    
            //         char *text = 0;
            //         int text_length = 0;
                    
            //         text = open_bracket.string+1;
            //         for(int i = 0; text[i]; ++i)
            //         {
            //             if(text[i] == '"')
            //             {
            //                 text = text+i+1;
            //                 break;
            //             }
            //         }
            //         for(text_length = 0; text[text_length] && text[text_length] != '"'; ++text_length);
                    
            //         ExprNode *node = ParseContextAllocateNode(context);
            //         node->type = ExprNodeType_Lister;
            //         node->string = text;
            //         node->string_length = text_length;
            //         node->text_style_flags = text_style_flags;
            //         *node_store_target = node;
            //         node_store_target = &(*node_store_target)->next;
                    
            //         tokenizer->at = text + text_length+1;
            //         if(!RequireToken(tokenizer, "}", 0))
            //         {
            //             PushParseError(context, tokenizer, "Expected } to follow lister data.");
            //         }
            //     }
            //     else
            //     {
            //         PushParseError(context, tokenizer, "A FeatureButton tag expects {<image>,<string>,<link>} to follow.");
            //     }
            // }
            
            else
            {
                PushParseError(context, tokenizer, "Malformed tag.");
            }
            
        }
        // else if(RequireTokenType(tokenizer, Token_Text, &text))
        // {
        //     ExprNode *node = ParseContextAllocateNode(context);
        //     node->type = ExprNodeType_Text;
        //     node->string = text.string;
        //     node->string_length = text.string_length;
        //     node->text_style_flags = text_style_flags;
        //     *node_store_target = node;
        //     node_store_target = &(*node_store_target)->next;
        // }
        else if(RequireTokenType(tokenizer, Token_Symbol, &symbol))
        {
            if(TokenMatch(symbol, "*"))
            {
                //multiply
            }
            else if(TokenMatch(symbol, "|"))
            {
                //or
            }
            else if(TokenMatch(symbol, "`"))
            {
                //String
            }
            else
            {
                PushParseError(context, tokenizer, "Unexpected symbol '%.*s'", symbol.string_length,
                               symbol.string);
            }
        }
        else if(RequireTokenType(tokenizer, Token_DoubleNewline, &text))
        {
            // ExprNode *node = ParseContextAllocateNode(context);
            // node->type = ExprNodeType_ParagraphBreak;
            // node->string = 0;
            // node->string_length = 0;
            // node->text_style_flags = text_style_flags;
            // *node_store_target = node;
            // node_store_target = &(*node_store_target)->next;
        }
        
        token = PeekToken(tokenizer);
        
        if(context->error_stack_size > 0)
        {
            break;
        }
    }
    
    return result;
}

typedef struct FileProcessData FileProcessData;
struct FileProcessData
{
    InputType input_type;
    OutputFlags output_flags;
    char *filename_no_extension;
    char *wasm_output_path;
    char *c_output_path;
    char *js_output_path;
};

typedef struct ProcessedFile ProcessedFile;
struct ProcessedFile
{
    ExprNode *root;
    char* wasm_file_contents;

    // File Data
    char *filename;
	
    int date_year;
    int date_month;
    int date_day;
    
    // General Input/Output Data
    InputType input_type;
    OutputFlags output_flags;
    
    // Wasm Output
    char *wasm_output_path;
    FILE *wasm_output_file;
    
    // @TODO: Other Formats
    char *c_output_path;
    FILE *c_output_file;
    char *js_output_path;
    FILE *js_output_file;
};
static const char* GetExprType(ExprType type){
    switch (type)
    {
    case ExprType_Var:
        return "Var";
    case ExprType_Const:
        return "Const";
    case ExprType_Func:
        return "Func";
    default:
        return "Invalid";
    }
}
static void
OutputWASMFromPageNodeTreeToFile(ExprNode *node, FILE *file, int follow_next, ProcessedFile *files, int file_count)
{
    ExprNode *previous_node = 0;
    
    for(; node; previous_node = node, node = node->next)
    {
        Log("%s",GetExprType(node->type));
        int i =0;
        while(i <= node->tokens_length){
            if(node->tokens[i].string_length > 0)
                Log("%s",node->tokens[i].string);
            i++;
        }
    }
}
static ProcessedFile
ProcessFile(char *filename, char *file, FileProcessData *process_data, ParseContext *context)
{
    ProcessedFile processed_file = {0};
    processed_file.filename = filename;
    processed_file.output_flags = process_data->output_flags;
    
    if(process_data->input_type == InputType_WASM)
    {
        processed_file.wasm_file_contents = file;
    }
    else if(process_data->input_type == InputType_OR)
    {
        Tokenizer tokenizer_ = {0};
        Tokenizer *tokenizer = &tokenizer_;
        tokenizer->at = file;
        tokenizer->line = 1;
        tokenizer->file = filename;
        
        ExprNode *page = ParseText(context, tokenizer);
        processed_file.root = page;
    }
    
    if(process_data->output_flags & OutputFlag_WASM)
    {
        processed_file.wasm_output_path = ParseContextAllocateCStringCopy(context, process_data->wasm_output_path);
        processed_file.wasm_output_file = fopen(process_data->wasm_output_path, "wb");
    }
    
    if(process_data->output_flags & OutputFlag_C)
    {
        processed_file.c_output_path = ParseContextAllocateCStringCopy(context, process_data->c_output_path);
        processed_file.c_output_file = fopen(process_data->c_output_path, "wb");
    }
    
    if(process_data->output_flags & OutputFlag_js)
    {
        processed_file.js_output_path = ParseContextAllocateCStringCopy(context, process_data->js_output_path);
        processed_file.js_output_file = fopen(process_data->js_output_path, "wb");
    }
    
    return processed_file;
}

static char *
LoadEntireFileAndNullTerminate(char *filename)
{
    char *result = 0;
    FILE *file = fopen(filename, "rb");
    if(file)
    {
        fseek(file, 0, SEEK_END);
        int file_size = ftell(file);
        fseek(file, 0, SEEK_SET);
        result = malloc(file_size+1);
        if(result)
        {
            fread(result, 1, file_size, file);
            result[file_size] = 0;
        }
    }
    return result;
}

static void
FreeFileData(void *data)
{
    free(data);
}

typedef struct KeywordPrefixTreeNode KeywordPrefixTreeNode;
struct KeywordPrefixTreeNode
{
    char *prefix;
    int prefix_length;
    char *value;
    int value_length;
    KeywordPrefixTreeNode *have_child;
    KeywordPrefixTreeNode *no_have_child;
};

void
InsertKeywordIntoTree(KeywordPrefixTreeNode **tree, ParseContext *context, char *key, int key_length, char *value, int value_length)
{
    KeywordPrefixTreeNode **node_target = tree;
    for(KeywordPrefixTreeNode *node = *tree;;)
    {
        int matching_key_characters = 0;
        
        if(node)
        {
            for(int i = 0; i < node->prefix_length && i < key_length; ++i)
            {
                if(key[i]  == node->prefix[i])
                {
                    ++matching_key_characters;
                }
                else
                {
                    break;
                }
            }
            
            // NOTE(rjf): We have matching characters, so this is either the node we want, or we want
            // to allocate a new node on the "have" child.
            if(matching_key_characters > 0)
            {
                if(node->prefix_length <= 1)
                {
                    node = node->have_child;
                    node_target = &node->have_child;
                }
                else
                {
                    KeywordPrefixTreeNode *new_node = ParseContextAllocateMemory(context, sizeof(*new_node));
                    new_node->have_child = node;
                    new_node->no_have_child = 0;
                    new_node->prefix = key;
                    new_node->prefix_length = matching_key_characters;
                    new_node->value = 0;
                    new_node->value_length = 0;
                    *node_target = new_node;
                    node = new_node->no_have_child;
                    node_target = &new_node->no_have_child;
                }
            }
            
            // NOTE(rjf): We don't have any matching characters, so move to the no-have child.
            else
            {
                node = node->no_have_child;
                node_target = &node->no_have_child;
            }
        }
        else
        {
            KeywordPrefixTreeNode *new_node = ParseContextAllocateMemory(context, sizeof(*new_node));
            new_node->have_child = 0;
            new_node->no_have_child = 0;
            new_node->prefix = key;
            new_node->prefix_length = key_length;
            new_node->value = value;
            new_node->value_length = value_length;
            *node_target = new_node;
            break;
        }
    }
}

int
GetKeywordValueFromTree(KeywordPrefixTreeNode *tree, char *keyword, int keyword_length, char **value_ptr)
{
    int value_length = 0;
    
    for(KeywordPrefixTreeNode *node = tree; node;)
    {
        int matching_key_characters = 0;
        
        for(int i = 0; i < node->prefix_length && i < keyword_length; ++i)
        {
            if(keyword[i] == node->prefix[i])
            {
                ++matching_key_characters;
            }
            else
            {
                break;
            }
        }
        
        if(matching_key_characters == node->prefix_length)
        {
            if(matching_key_characters == keyword_length)
            {
                if(value_ptr)
                {
                    *value_ptr = node->value;
                    value_length = node->value_length;
                    break;
                }
            }
            else
            {
                node = node->have_child;
            }
        }
        else
        {
            node = node->no_have_child;
        }
    }
    
    return value_length;
}

KeywordPrefixTreeNode *
GenerateKeywordPrefixTreeFromFile(ParseContext *context, char *filename)
{
    KeywordPrefixTreeNode *root = 0;
    
    char *file = LoadEntireFileAndNullTerminate(filename);
    
    for(int i = 0; file[i]; ++i)
    {
        char *key = 0;
        int key_length = 0;
        char *value = 0;
        int value_length = 0;
        if(!CharIsSpace(file[i]))
        {
            key = file+i+1;
            for(; key[key_length] != ':'; ++key_length);
            value = file+i+key_length+1;
            for(int j = 0; CharIsSpace(file[i+key_length+j]); ++j, ++value);
            for(; value[value_length] != '\n'; ++value_length);
            InsertKeywordIntoTree(&root, context, key, key_length, value, value_length);
        }
    }
    
    FreeFileData(file);
    
    return root;
}

#define MAX_FILE_COUNT 4096

/**
 * Lists all files and sub-directories recursively 
 * considering path as base path.
 */
void listFilesRecursively(char *basePath,char** filenames,int* file_count)
{
    char path[1000];
    struct dirent *dp;
    DIR *dir = opendir(basePath);

    // Unable to open directory stream
    if (!dir)
        return;

    while ((dp = readdir(dir)) != NULL)
    {
        if (strcmp(dp->d_name, ".") != 0 && strcmp(dp->d_name, "..") != 0)
        {

            // Construct new path from our base path
            strcpy(path, basePath);
            strcat(path, "/");
            strcat(path, dp->d_name);
			struct stat sb;
			if(stat(path, &sb) == 0 && S_ISREG(sb.st_mode))
            {
                filenames[*file_count] = malloc(sizeof(path));
				strcpy(filenames[*file_count],path);
				*file_count+=1;
            }
            if(*file_count >= MAX_FILE_COUNT)
            {
                fprintf(stderr, "ERROR: Max file count reached. @John, increase this.\n");
				break;
            }
            listFilesRecursively(path,filenames,file_count);
        }
    }

    closedir(dir);
}

int
main(int argument_count, char **arguments)
{
    int expected_file_count = 0;
    OutputFlags output_flags = 0;
	char *source_dir_path = 0;
    char *build_file_path = 0;
	char *build_file = "";
    
    for(int i = 1; i < argument_count; ++i)
    {
        
        if(CStringMatchCaseInsensitive(arguments[i], "--wasm"))
        {
            Log("Outputting to WASM.");
            output_flags |= OutputFlag_WASM;
            arguments[i] = 0;
        }
        else if(CStringMatchCaseInsensitive(arguments[i], "--c"))
        {
            Log("Outputting to C.");
            output_flags |= OutputFlag_C;
            arguments[i] = 0;
        }
        else if(CStringMatchCaseInsensitive(arguments[i], "--js"))
        {
            Log("Outputting to js.");
            output_flags |= OutputFlag_js;
            arguments[i] = 0;
        }
        
        //Arguments with input data (not just flags).
        else if(argument_count > i+1)
        {
            if(CStringMatchCaseInsensitive(arguments[i], "--source") || CStringMatchCaseInsensitive(arguments[i], "-s"))
            {
                source_dir_path = arguments[i+1];
                Log("Source files Directory set as \"%s\".", source_dir_path);
                arguments[i] = 0;
                arguments[i+1] = 0;
                ++i;
            }
            else if(CStringMatchCaseInsensitive(arguments[i], "--build_file") || CStringMatchCaseInsensitive(arguments[i], "-b"))
            {
                build_file_path = arguments[i+1];
                Log("Build file set as \"%s\".", build_file_path);
                arguments[i] = 0;
                arguments[i+1] = 0;
                ++i;
            }
        }
        // NOTE(rjf): Just a file to parse.
        else
        {
            ++expected_file_count;
        }
        
    }
    
    if(build_file_path)
    {
        build_file = LoadEntireFileAndNullTerminate(build_file_path);
    }
    
    ParseContext context = {0};
    ProcessedFile files[MAX_FILE_COUNT];
	char* filenames[MAX_FILE_COUNT] ={0};
    int file_count = 0;
	if(source_dir_path){
		listFilesRecursively(source_dir_path,&filenames,&file_count);
	}
    for(int i = 0; i < file_count; i++)
    {
        char *filename = filenames[i];
        if(filename)
        {
            Log("Processing file \"%s\".", filename);
        
            char extension[256] = {0};
            char filename_no_extension[256] = {0};
            char wasm_output_path[256] = {0};
            char c_output_path[256] = {0};
            char js_output_path[256] = {0};
            
            snprintf(filename_no_extension, sizeof(filename_no_extension), "%s", filename);
            char *last_period = filename_no_extension;
            for(int i = 0; filename_no_extension[i]; ++i)
            {
                if(filename_no_extension[i] == '.')
                {
                    last_period = filename_no_extension+i;
                }
            }
            *last_period = 0;
            
            snprintf(wasm_output_path, sizeof(wasm_output_path), "%s.wasm", filename_no_extension);
            snprintf(c_output_path, sizeof(c_output_path), "%s.c", filename_no_extension);
            snprintf(js_output_path, sizeof(js_output_path), "%s.js", filename_no_extension);
            
            snprintf(extension, sizeof(extension), "%s", last_period+1);
            InputType input_type = InputType_Invalid;
            if(CStringMatchCaseInsensitive(extension, "or"))
            {
                input_type = InputType_OR;
            }
			else if(CStringMatchCaseInsensitive(extension, "wasm")){
				input_type = InputType_WASM;
			}
            
            FileProcessData process_data = {0};
            {
                process_data.input_type = input_type;
                process_data.output_flags = output_flags;
                process_data.filename_no_extension = filename_no_extension;
                process_data.wasm_output_path = wasm_output_path;
                process_data.c_output_path = c_output_path;
                process_data.js_output_path = js_output_path;
            }
            
            ProcessedFile processed_file = {0};
			if(input_type != InputType_Invalid){
				char *file = LoadEntireFileAndNullTerminate(filename);
				processed_file = ProcessFile(filename, file, &process_data, &context);
			}
			else
            {
                fprintf(stderr, "ERROR: input file %s.%s is not a valid file type; Only .wasm and .or are supported\n",filename,extension);
				continue;
            }
            
            if(file_count < sizeof(files)/sizeof(files[0]))
            {
                files[file_count++] = processed_file;
            }
            else
            {
                fprintf(stderr, "ERROR: Max file count reached. @John, increase this.\n");
            }
        }
    }
    
    // Print errors.
    if(context.error_stack_size > 0)
    {
        for(int i = 0; i < context.error_stack_size; ++i)
        {
            fprintf(stderr, "Parse Error (%s:%i): %s\n",
                    context.error_stack[i].file,
                    context.error_stack[i].line,
                    context.error_stack[i].message);
        }
    }
    
    //Generate code for all processed files.
    {
        for(int i = 0; i < file_count; ++i)
        {
            ProcessedFile *file = files+i;
            
            if(file->wasm_output_file)
            {
                if(file->root)
                {
                    OutputWASMFromPageNodeTreeToFile(file->root, file->wasm_output_file,1,files, file_count);
                }
                else if(file->wasm_file_contents)
                {
                    fprintf(file->wasm_output_file, "%s", file->wasm_file_contents);
                }
            }
            
            if(file->c_output_file)
            {
                // @TODO: jsn
            }
            
            if(file->js_output_file)
            {
                // @TODO: jsn
            }
        }
    }
	for(int i =0; i < file_count;i++){
		free(filenames[i]);
	}
    
    return 0;
}
