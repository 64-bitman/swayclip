import sys
import os

def to_c_array(input_path, output_path, array_name):
    with open(input_path, 'rb') as f:
        data = f.read()
    data.decode('utf-8')  # validate
    data += b'\x00'

    with open(output_path, 'w') as f:
        f.write(f"static const char {array_name}[] = {{\n")
        for i in range(0, len(data), 12):
            chunk = data[i:i+12]
            f.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
        f.write("};\n")

os.chdir(os.path.dirname(os.path.realpath(__file__)))
to_c_array("db_schema.sql", "db_schema.h", "db_schema")
