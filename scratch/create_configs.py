import os

base_path = r"C:\Users\rizki\Programming\vfrns\vfr_switch\test_config_pvc-1.conf"
out_dir = r"C:\Users\rizki\Programming\vfrns\vfr_switch"

with open(base_path, "r", encoding="utf-8") as f:
    content = f.read()

# Config 1: Both t392=20
content_1 = content.replace("log_file vfrsA.log", "log_file vfrs_slow_1.log")
content_1 = content_1.replace("lmi uni0/0 q933a t392=45", "lmi uni0/0 q933a t392=20")
content_1 = content_1.replace("lmi uni0/1 q933a t392=45", "lmi uni0/1 q933a t392=20")
with open(os.path.join(out_dir, "test_slow_1.conf"), "w", encoding="utf-8") as f:
    f.write(content_1)

# Config 2: uni0/0 t392=45, uni0/1 t392=20
content_2 = content.replace("log_file vfrsA.log", "log_file vfrs_slow_2.log")
content_2 = content_2.replace("lmi uni0/1 q933a t392=45", "lmi uni0/1 q933a t392=20")
# Note: uni0/0 t392=45 is left unchanged
with open(os.path.join(out_dir, "test_slow_2.conf"), "w", encoding="utf-8") as f:
    f.write(content_2)

# Config 3: uni0/0 t392=20, uni0/1 t392=45
content_3 = content.replace("log_file vfrsA.log", "log_file vfrs_slow_3.log")
content_3 = content_3.replace("lmi uni0/0 q933a t392=45", "lmi uni0/0 q933a t392=20")
# Note: uni0/1 t392=45 is left unchanged
with open(os.path.join(out_dir, "test_slow_3.conf"), "w", encoding="utf-8") as f:
    f.write(content_3)

print("Generated test_slow_1.conf, test_slow_2.conf, and test_slow_3.conf successfully.")
