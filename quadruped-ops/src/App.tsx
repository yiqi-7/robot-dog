import { Routes, Route } from "react-router";
import { Toaster } from "@/components/ui/sonner";
import { SimProvider } from "@/sim/store";
import { TaskProvider } from "@/providers/trpc";
import { AppShell } from "@/components/layout/AppShell";
import Home from "./pages/Home";
import Robots from "./pages/Robots";
import Tasks from "./pages/Tasks";
import Alarms from "./pages/Alarms";

export default function App() {
  return (
    <SimProvider>
      <TaskProvider>
        <AppShell>
          <Routes>
            <Route path="/" element={<Home />} />
            <Route path="/robots" element={<Robots />} />
            <Route path="/tasks" element={<Tasks />} />
            <Route path="/alarms" element={<Alarms />} />
            <Route path="*" element={<Home />} />
          </Routes>
        </AppShell>
        {/*
          提示框改到右下角：原先 top-center 正好压住顶栏导航
          （总览 / 机器人 / 任务管理 / 告警中心），提示出现期间点不到导航。
          同时开启关闭按钮，提示可手动收起。
        */}
        <Toaster theme="dark" position="bottom-right" closeButton toastOptions={{ duration: 3500 }} />
      </TaskProvider>
    </SimProvider>
  );
}
