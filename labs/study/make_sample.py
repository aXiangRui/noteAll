"""生成一份仿真课件（用于验证管道，不依赖用户数据）。

真实课件长什么样，这里就长什么样：标题 + 要点 + 演讲者备注，偶尔一页表格。
"""
from pptx import Presentation
from pptx.util import Inches, Pt
import sys

def add_slide(prs, title, bullets, notes=""):
    layout = prs.slide_layouts[1]  # 标题和内容
    slide = layout or prs.slide_layouts[6]
    slide = prs.slides.add_slide(layout)
    slide.shapes.title.text = title
    body = slide.placeholders[1]
    tf = body.text_frame
    tf.text = bullets[0]
    for line in bullets[1:]:
        p = tf.add_paragraph()
        p.text = line
    if notes:
        slide.notes_slide.notes_text_frame.text = notes

def main(path):
    prs = Presentation()

    add_slide(prs, "第三章 进程调度", [
        "3.1 调度基本概念",
        "3.2 批处理调度算法",
        "3.3 交互式调度算法",
        "3.4 实时调度",
    ], "本章是考试重点，平均周转时间计算方法必须掌握。例题 3-2 与作业题 5 同类型。")

    add_slide(prs, "3.1 为什么需要调度", [
        "CPU 是稀缺资源，进程数远多于 CPU 数",
        "调度的目标：吞吐量、响应时间、公平性、利用率",
        "不同系统目标互相冲突：批处理重吞吐，交互式重响应",
    ], "重点：四个调度目标之间的冲突，常考简答题。")

    add_slide(prs, "3.1 调度的三个层次", [
        "高级调度（作业调度）：决定哪些作业进入内存",
        "中级调度（内存调度）：挂起与激活，调节多道度",
        "低级调度（进程调度）：从就绪队列选一个上 CPU",
        "低级调度频率最高，必须极快",
    ], "三级调度的区别是常考选择题，注意「频率」这个判断依据。")

    add_slide(prs, "3.2 先来先服务 FCFS", [
        "按到达顺序执行，非抢占",
        "优点：简单、公平（对长作业友好）",
        "缺点：护航效应（convoy effect）——短作业排在长作业后面",
        "平均周转时间通常较差",
    ], "护航效应必考，要能举例说明：一个长作业后面跟十个短作业。")

    add_slide(prs, "3.2 短作业优先 SJF", [
        "选择估计运行时间最短的作业",
        "在非抢占下平均周转时间最优（可证明）",
        "缺点：需要预知运行时间；长作业可能饥饿",
        "实际系统用指数平均预测下一次 CPU 突发",
    ], "★ 必考：为什么 SJF 平均周转时间最优（证明思路，不要求完整证明）。")

    add_slide(prs, "3.2 例题 3-2：平均周转时间", [
        "作业 A：到达 0，运行 7",
        "作业 B：到达 2，运行 4",
        "作业 C：到达 4，运行 1",
        "求 FCFS 与 SJF 的平均周转时间",
    ], "例题 3-2 逐字看一遍。周转时间 = 完成时间 - 到达时间；平均周转时间 = 总和 / 作业数。")

    add_slide(prs, "3.3 时间片轮转 RR", [
        "就绪队列 FIFO，每个进程最多运行一个时间片",
        "时间片过大退化为 FCFS；过小则切换开销占比高",
        "响应时间有保证，适合分时系统",
        "通常取 10–100 ms",
    ], "重点：时间片大小对性能的影响，这是 RR 的核心考点。")

    add_slide(prs, "3.3 优先级调度", [
        "每个进程有优先级，最高优先级先运行",
        "静态优先级 vs 动态优先级",
        "抢占式 vs 非抢占式",
        "问题：低优先级进程饥饿 → 用老化（aging）解决",
    ], "老化（aging）的定义必背，简答题常问「如何解决饥饿」。")

    add_slide(prs, "3.3 多级反馈队列 MLFQ", [
        "多个就绪队列，优先级从高到低，时间片从短到长",
        "新进程进最高优先级队列",
        "用完时间片则降级；因 I/O 阻塞则留在原级",
        "折中方案：兼顾响应时间与吞吐量",
    ], "MLFQ 是综合题素材，要能画出队列结构与升降级规则。")

    add_slide(prs, "3.4 实时调度", [
        "硬实时 vs 软实时",
        "EDF：最早截止期优先，CPU 利用率上限可证明",
        "RMS：速率单调，周期短的优先级高",
        "实时系统必须可预测，不能用平均性能衡量",
    ], "了解即可，考大题概率低，但 EDF 与 RMS 的名字要会。")

    add_slide(prs, "本章小结", [
        "调度目标之间存在冲突，没有全能算法",
        "批处理看周转时间，交互式看响应时间",
        "SJF 最优但不可实现，MLFQ 是工程折中",
        "平均周转时间计算：完成减到达",
    ], "复习建议：把例题 3-2 自己算一遍，再对照答案。")

    prs.save(path)
    print(f"已生成: {path}  共 {len(prs.slides.__iter__.__self__._sldIdLst)} 页")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "sample-course.pptx")
