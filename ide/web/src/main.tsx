import './storageRename';     // first: carries saved state over from the old ledctl.ide.* keys
import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './App';
import './app.css';

createRoot(document.getElementById('root')!).render(<StrictMode><App /></StrictMode>);
