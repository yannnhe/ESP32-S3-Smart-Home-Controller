# 源文件目录
set(src_dirs

)

# 头文件路径
set(include_dirs

)

# 依赖的组件（ESP-IDF 内置组件/自定义组件）
set(requires
            driver # 外设驱动（基础依赖）
)

# 最终在组件注册时使用（核心）
idf_component_register(
                        SRC_DIRS ${src_dirs} # 展开所有源文件目录
                        INCLUDE_DIRS ${include_dirs} # 展开所有头文件目录
                        REQUIRES ${requires} # 展开所有依赖组件
)