function embed_file(input_path, output_path, var_name)
  local in_f = io.open(input_path, "rb")
  if not in_f then error("Could not open input: " .. input_path) end
  local data = in_f:read("*all")
  in_f:close()

  local out_f = io.open(output_path, "w")
  out_f:write("static unsigned char " .. var_name .. "[] = {\n")
  
  local bytes = {}
  for i = 1, #data do
      table.insert(bytes, string.format("0x%02x", string.byte(data, i)))
  end
  
  out_f:write(table.concat(bytes, ", "))
  out_f:write("\n};\n")
  out_f:write("static unsigned int " .. var_name .. "_len = " .. #data .. ";\n")
  out_f:close()
  
  print("Generated embedded header: " .. output_path)
end