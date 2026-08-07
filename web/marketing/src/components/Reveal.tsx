import { motion, type Variants } from "framer-motion";
import type { PropsWithChildren } from "react";

const containerVariants: Variants = {
  hidden: {},
  show: {
    transition: { staggerChildren: 0.09, delayChildren: 0.05 },
  },
};

/** Wraps a section's children so they stagger-reveal once scrolled into view. */
export function RevealGroup({
  children,
  className,
  id,
}: PropsWithChildren<{ className?: string; id?: string }>) {
  return (
    <motion.div
      className={className}
      id={id}
      variants={containerVariants}
      initial="hidden"
      whileInView="show"
      viewport={{ once: true, amount: 0.2 }}
    >
      {children}
    </motion.div>
  );
}

const itemVariants: Variants = {
  hidden: { opacity: 0, y: 26 },
  show: { opacity: 1, y: 0, transition: { duration: 0.5, ease: "easeOut" } },
};

export function RevealItem({
  children,
  className,
}: PropsWithChildren<{ className?: string }>) {
  return (
    <motion.div className={className} variants={itemVariants}>
      {children}
    </motion.div>
  );
}
