#include "front.h"

static int read_source(Context *c, const char *path, ncc_buffer *source) {
    ncc_status status;
    c->path = path; c->line = 0;
    status = ncc_read_file(c->platform,path,source);
    if (status != NCC_OK) { tool_error(c,ncc_status_string(status)); return 0; }
    for (size_t i = 0; i < source->size; ++i) if (!source->data[i]) { tool_error(c,"NUL byte in source file"); return 0; }
    return tool_byte(c,source,0);
}
static const char *default_output(Context *c, const char *input, const char *extension) {
    size_t n = ncc_strlen(input), start = 0, dot = n;
    char *s;
    for (size_t i = 0; i < n; ++i) { if (input[i] == '/') { start = i+1; dot = n; } else if (input[i] == '.') dot = i; }
    if (dot < start) dot = n;
    s = tool_alloc(c,dot-start+ncc_strlen(extension)+1);
    if (s) { ncc_memcpy(s,input+start,dot-start); ncc_memcpy(s+dot-start,extension,ncc_strlen(extension)+1); }
    return s;
}
static int ends(const char *s, const char *suffix) {
    size_t a = ncc_strlen(s), b = ncc_strlen(suffix);
    return a >= b && !ncc_memcmp(s+a-b,suffix,b);
}
int main(int argc, char **argv) {
    static const char startup[] = ".text\n.global _start\n_start:\nmov rdi, qword [rsp]\nlea rsi, [rsp+8]\nand rsp, -16\ncall main\nmov edi, eax\nmov eax, 60\nsyscall\n";
    Context c;
    ncc_buffer input, output, assembly, predefines, *objects = NULL;
    const char *destination = NULL, *entry = "_start", **paths, **object_paths;
    const char *program = argv[0];
    size_t count = 0, object_count = 0;
    int linker, compiler, result = 1, mode = 0, optimization = 0;
    tool_init(&c);
    ncc_buffer_init(&predefines,c.platform);
    ncc_buffer_init(&input,c.platform); ncc_buffer_init(&output,c.platform); ncc_buffer_init(&assembly,c.platform);
    for (const char *p = program; *p; ++p) if (*p == '/') program = p+1;
    linker = tool_equal(program,"nld"); compiler = tool_equal(program,"ncc");
    paths = tool_alloc(&c,((size_t)argc+1)*sizeof(*paths));
    object_paths = tool_alloc(&c,((size_t)argc+1)*sizeof(*object_paths));
    c.include_paths = tool_alloc(&c,((size_t)argc+1)*sizeof(*c.include_paths));
    if (!paths || !object_paths || !c.include_paths) goto done;
    for (int i = 1; i < argc; ++i) {
        if (tool_equal(argv[i],"-o") || (linker && tool_equal(argv[i],"-e"))) {
            int is_entry = argv[i][1] == 'e';
            if (++i == argc) { tool_error(&c,"option requires an argument"); goto done; }
            if (is_entry) entry = argv[i]; else destination = argv[i];
        } else if (compiler && (tool_equal(argv[i],"-c") || tool_equal(argv[i],"-S") || tool_equal(argv[i],"-E"))) {
            if (mode) { tool_error(&c,"output modes are mutually exclusive"); goto done; }
            mode = argv[i][1];
        } else if (compiler && (tool_equal(argv[i],"-O0") || tool_equal(argv[i],"-O1") || tool_equal(argv[i],"-O2") || tool_equal(argv[i],"-Os"))) optimization = argv[i][2] == 's' ? 3 : argv[i][2]-'0';
        else if (compiler && (argv[i][1] == 'I' || argv[i][1] == 'D' || argv[i][1] == 'U') && argv[i][0] == '-') {
            char option = argv[i][1];
            const char *value = argv[i]+2;
            if (!*value) {
                if (++i == argc) { tool_error(&c,"option requires an argument"); goto done; }
                value = argv[i];
            }
            if (option == 'I') c.include_paths[c.include_count++] = value;
            else {
                size_t n = ncc_strlen(value), equal = n;
                for (size_t j = 0; j < n; ++j) if (value[j] == '=') { equal = j; break; }
                tool_append(&c,&predefines,option == 'D' ? "#define " : "#undef ",option == 'D' ? 8 : 7);
                tool_append(&c,&predefines,value,equal);
                if (option == 'D') {
                    tool_byte(&c,&predefines,' ');
                    if (equal < n) tool_append(&c,&predefines,value+equal+1,n-equal-1);
                    else tool_byte(&c,&predefines,'1');
                }
                tool_byte(&c,&predefines,'\n');
            }
        }
        else if (compiler && (tool_equal(argv[i],"-std=c99") || tool_equal(argv[i],"-ffreestanding"))) continue;
        else if (tool_equal(argv[i],"--help")) {
            const char *help = linker ? "Usage: nld [-o executable] [-e entry] object...\n" : compiler ? "Usage: ncc [-c|-S|-E] [-O0|-O1|-O2|-Os] [-o output] source...\n" : "Usage: nas [-o object] source\n";
            ncc_write_all(c.platform,1,help,ncc_strlen(help)); result = 0; goto done;
        } else if (argv[i][0] == '-') { tool_error(&c,"unknown option"); goto done; }
        else paths[count++] = argv[i];
    }
    if (compiler) {
        size_t slash = 0;
        ncc_buffer directory;
        for (size_t i = 0; argv[0][i]; ++i) if (argv[0][i] == '/') slash = i+1;
        ncc_buffer_init(&directory,c.platform);
        tool_append(&c,&directory,argv[0],slash); tool_append(&c,&directory,"include/c99",11);
        c.include_paths[c.include_count++] = tool_string(&c,(char *)directory.data,directory.size);
        ncc_buffer_destroy(&directory);
        tool_byte(&c,&predefines,0); c.predefines = (char *)predefines.data;
    }
    if (!count) { tool_error(&c,"no input files"); goto done; }
    if (linker) {
        if (!destination) destination = "a.out";
        if (!nld_link(&c,paths,count,entry,&output)) goto done;
    } else if (!compiler) {
        if (count != 1) { tool_error(&c,"expected exactly one assembly input file"); goto done; }
        if (!destination) destination = default_output(&c,paths[0],".o");
        if (!read_source(&c,paths[0],&input) || !nas_assemble(&c,(char *)input.data,&output)) goto done;
    } else {
        if (mode && count != 1) { tool_error(&c,"this output mode requires one input file"); goto done; }
        objects = tool_alloc(&c,(count+1)*sizeof(*objects));
        if (!objects) goto done;
        for (size_t i = 0; i <= count; ++i) ncc_buffer_init(&objects[i],c.platform);
        for (size_t i = 0; i < count; ++i) {
            ncc_buffer_destroy(&input); ncc_buffer_destroy(&assembly);
            c.path = paths[i]; c.line = 0;
            object_paths[object_count] = paths[i];
            if ((ends(paths[i],".o") || ends(paths[i],".a")) && !mode) {
                ncc_status s = ncc_read_file(c.platform,paths[i],&objects[object_count]);
                if (s != NCC_OK) { tool_error(&c,ncc_status_string(s)); goto done; }
            } else {
                if (!read_source(&c,paths[i],&input)) goto done;
                if (mode == 'E') {
                    Token *tokens = front_preprocess(&c,(char *)input.data,paths[i]);
                    if (c.failed) goto done;
                    for (Token *t = tokens; t && t->kind; t = t->next) {
                        if (!tool_append(&c,&output,t->text,t->length) || !tool_byte(&c,&output,' ')) goto done;
                    }
                    if (!tool_byte(&c,&output,'\n')) goto done;
                    if (!destination) { result = ncc_write_all(c.platform,1,output.data,output.size) != NCC_OK; goto done; }
                    break;
                }
                if (!ncc_compile(&c,(char *)input.data,paths[i],optimization,&assembly)) goto done;
                if (mode == 'S') {
                    output = assembly; ncc_buffer_init(&assembly,c.platform);
                    if (!destination) destination = default_output(&c,paths[i],".s");
                    break;
                }
                if (!tool_byte(&c,&assembly,0) || !nas_assemble(&c,(char *)assembly.data,&objects[object_count])) goto done;
            }
            ++object_count;
        }
        if (mode == 'c') {
            output = objects[0]; ncc_buffer_init(&objects[0],c.platform);
            if (!destination) destination = default_output(&c,paths[0],".o");
        } else if (!mode) {
            c.path = "<startup>";
            if (!nas_assemble(&c,startup,&objects[object_count])) goto done;
            object_paths[object_count++] = "<startup>";
            if (!nld_link_memory(&c,object_paths,objects,object_count,entry,&output)) goto done;
            if (!destination) destination = "a.out";
        }
    }
    c.path = destination; c.line = 0;
    if (destination && tool_save(&c,destination,&output,linker || (compiler && !mode))) result = 0;
done:
    if (objects) for (size_t i = 0; i <= count; ++i) ncc_buffer_destroy(&objects[i]);
    ncc_buffer_destroy(&predefines); ncc_buffer_destroy(&input); ncc_buffer_destroy(&output); ncc_buffer_destroy(&assembly);
    tool_destroy(&c);
    return result;
}
