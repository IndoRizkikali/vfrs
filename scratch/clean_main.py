import re

with open('src/main.c', 'r', encoding='utf-8') as f:
    content = f.read()

# Replace from '/* Parse configuration file */\nstatic int parse_config' to the second '/* Parse configuration file */'
pattern = re.compile(r'/\* Parse configuration file \*/\s*static int parse_config\(vfrs_ctx_t \*ctx, const char \*filename\)\s*\{.*?config_destroy\(cfg\);\s*return 0;\s*\}\s*(?=/\* Parse configuration file \*/)', re.DOTALL)

new_content, count = pattern.subn('', content, count=1)
print(f"Substituted {count} matches")

with open('src/main.c', 'w', encoding='utf-8') as f:
    f.write(new_content)
