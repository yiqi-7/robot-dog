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
        <Toaster theme="dark" position="top-center" />
      </TaskProvider>
    </SimProvider>
  );
}
