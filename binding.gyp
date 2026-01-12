{
  "targets": [
    {
      "target_name": "opcda",
      "sources": [ "src/opcda.cc" ],
      "include_dirs": [
        "include",
        "<!@(node -p \"require('node-addon-api').include\")",
        "<!@(node -p \"require('node-addon-api').include_dir\")"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "cflags": [ "-std=c++17" ],
      "conditions": [
        ['OS=="win"', {
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": "2",
              "DisableSpecificWarnings": [ "4530", "4506", "4996" ],
              "AdditionalOptions": [ "/std:c++17" ]
            },
            "VCLinkerTool": {
              "AdditionalOptions": [ "/HIGHENTROPYVA:NO" ],
              "AdditionalDependencies": [ "OPCClientToolKit64.lib" ],
              "AdditionalLibraryDirectories": [ "lib/x64" ]
            }
          }
        }]
      ],
      "libraries": [ "lib/x64/OPCClientToolKit64.lib" ]
    }
  ]
}