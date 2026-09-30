#pragma once

/* 描述: 添加水印 */
/* StartAddress:	水印起始地址 */
/* EndAddress:		水印结束地址 */
/* 返回值: 是否添加成功; false:添加失败! */
typedef bool (*pfn_AddMarks)(size_t StartAddress, size_t EndAddress);

