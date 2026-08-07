import { motion } from "framer-motion";
import type { PropsWithChildren } from "react";

/** The terminal/editor-chrome code block used across the API reference sections. */
export function CodePanel({ filename, children }: PropsWithChildren<{ filename: string }>) {
  return (
    <motion.div
      className="code-panel glass"
      whileHover={{ rotate: -0.2, scale: 1.005 }}
      transition={{ type: "spring", stiffness: 260, damping: 22 }}
    >
      <div className="code-panel-head">
        <span style={{ background: "#ff6459" }} />
        <span style={{ background: "#ffbd2e" }} />
        <span style={{ background: "#28c840" }} />
        <span className="filename">{filename}</span>
      </div>
      <pre>
        <code>{children}</code>
      </pre>
    </motion.div>
  );
}
