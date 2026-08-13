# 添加C++支持过程中遇到的问题

## 编译报错，找不到符号：_sinit/_einit

目前解决方法是：修改ldscript，手动添加这两个符号：
```diff
diff --git a/board/esp32p4/common/scripts/esp32p4_sections.ld b/board/esp32p4/common/scripts/esp32p4_sections.ld
index 9733978..00ff6fc 100644
--- a/board/esp32p4/common/scripts/esp32p4_sections.ld
+++ b/board/esp32p4/common/scripts/esp32p4_sections.ld
@@ -586,6 +586,8 @@ SECTIONS
      * startup. The corresponding code can be found in startup.c.
      */
 
+  _sinit = ABSOLUTE(.);
+
  . = ALIGN(4);
  __init_priority_array_start = ABSOLUTE(.);
     KEEP (*(EXCLUDE_FILE (*crtend.* *crtbegin.*) .init_array.*))
@@ -597,6 +599,8 @@ SECTIONS
     __init_array_end = ABSOLUTE(.);
     /* Addresses of memory regions reserved via SOC_RESERVE_MEMORY_REGION() */
 
+  _einit = ABSOLUTE(.);
+
  . = ALIGN(4);
  soc_reserved_memory_region_start = ABSOLUTE(.);
     KEEP (*(.reserved_memory_address))
diff --git a/board/esp32p4/common/scripts/esp32p4_sections.rev3.ld b/board/esp32p4/common/scripts/esp32p4_sections.rev3.ld
index 8afb4d1..f49208c 100644
--- a/board/esp32p4/common/scripts/esp32p4_sections.rev3.ld
+++ b/board/esp32p4/common/scripts/esp32p4_sections.rev3.ld
@@ -652,6 +652,8 @@ SECTIONS
      * startup. The corresponding code can be found in startup.c.
      */
 
+  _sinit = ABSOLUTE(.);
+
  . = ALIGN(4);
  __init_priority_array_start = ABSOLUTE(.);
     KEEP (*(EXCLUDE_FILE (*crtend.* *crtbegin.*) .init_array.*))
@@ -663,6 +665,8 @@ SECTIONS
     __init_array_end = ABSOLUTE(.);
     /* Addresses of memory regions reserved via SOC_RESERVE_MEMORY_REGION() */
 
+  _einit = ABSOLUTE(.);
+
  . = ALIGN(4);
  soc_reserved_memory_region_start = ABSOLUTE(.);
     KEEP (*(.reserved_memory_address))
```

但不确定是否会和`__init_array_start`初始化行为重复。需要进一步追踪代码流程和编译选项。
