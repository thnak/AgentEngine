import { Architecture } from "./components/Architecture";
import { Footer } from "./components/Footer";
import { GettingStarted } from "./components/GettingStarted";
import { Hero } from "./components/Hero";
import { Invariants } from "./components/Invariants";
import { Nav } from "./components/Nav";
import { Pillars } from "./components/Pillars";
import { SpecDriven } from "./components/SpecDriven";
import { LanguageProvider } from "./i18n/LanguageContext";

function App() {
  return (
    <LanguageProvider>
      <Nav />
      <main>
        <Hero />
        <Pillars />
        <Invariants />
        <Architecture />
        <SpecDriven />
        <GettingStarted />
      </main>
      <Footer />
    </LanguageProvider>
  );
}

export default App;
