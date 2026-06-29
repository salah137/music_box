file(REMOVE_RECURSE
  "../../esp_err_codes/ulp_err_codes.csv"
  "../../esp_err_codes/ulp_esp_err_codes.c"
)

# Per-language clean rules from dependency scanning.
foreach(lang )
  include(CMakeFiles/__idf_ulp.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
